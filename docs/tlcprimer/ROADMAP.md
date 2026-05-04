# Thylacine OS — Roadmap

**Status**: Phase 0. This document translates `ARCHITECTURE.md` committed decisions
into a staged implementation plan. Time estimates are guidance, not commitments.

---

## 1. Purpose

ROADMAP.md answers:
- What do we build, in what order, and why?
- What are each phase's exit criteria?
- Where are the risk hot spots?
- How does Stratum integrate at each phase?

The roadmap is a commitment to an *ordering*, not to specific dates. Phases can overlap
where dependencies allow. The Stratum lesson: month-based estimates are fiction; phase
ordering is not.

---

## 2. Phase structure at a glance

```
Phase 0 ─ Design (DONE — this document)
          VISION.md, ARCHITECTURE.md, ROADMAP.md

Phase 1 ─ Kernel skeleton                      [foundation]
          Boot, memory, exception vectors, UART

Phase 2 ─ Process model + scheduler            [core kernel]
          rfork, exec, namespace primitives, preemptive scheduler

Phase 3 ─ Device model + VirtIO                [device layer]
          Dev vtable, Chan, VirtIO core, block + net drivers

Phase 4 ─ Filesystem + Stratum integration     [storage]
          ramfs, 9P client, Stratum daemon mount, janus

Phase 5 ─ Syscall surface + musl port          [userspace path]
          Full syscall table, musl libc port, shell programs run

Phase 6 ─ Halcyon                              [shell]
          Framebuffer driver, graphical scroll-buffer shell, full usability

Phase 7 ─ Compatibility layer                  [compat]
          Linux ARM64 binary compat, namespace compat, container-as-namespace

Phase 8 ─ Hardening + v1.0                     [release]
          Fuzz, audit, benchmarks, docs, tag
```

---

## 3. Principles throughout

### 3.1 Audit loop from Phase 1

Every change to exception handling, memory management, namespace operations, credential
handling, and crypto surfaces spawns a focused soundness audit before merge. Modeled on
Stratum's 15-round audit history. Findings at P0/P1 severity block phase exit.

### 3.2 Tests are tiered

- **Unit tests**: per-function, per-syscall.
- **Integration tests**: boot a QEMU instance, run a workload, check result.
- **Stress tests**: long-running, with process creation/destruction, namespace
  manipulation, and I/O concurrency.

### 3.3 Stratum is the reference deployment environment

Once Phase 4 lands, all development work happens inside a Thylacine QEMU VM with
Stratum as the root filesystem. Eating the dog food from Phase 4 onward.

### 3.4 No in-kernel drivers unless necessary

If a device can be driven from userspace via VirtIO + 9P, it is. Kernel drivers are
the last resort. The boundary is: interrupt latency and DMA must be in-kernel;
protocol logic and device state can be in userspace.

### 3.5 The compat layer is built on top, not baked in

The kernel API is Thylacine-native throughout. POSIX/Linux compatibility is implemented
as a userspace library (musl) and a thin syscall translation shim. The kernel is never
modified to accommodate POSIX requirements.

---

## 4. Phase 1: Kernel skeleton

**Goal**: the kernel boots in QEMU `virt`, initializes hardware, and drops to a debug
shell via UART. No processes, no virtual memory, no devices beyond UART.

### 4.1 Deliverables

- `arch/arm64/start.S`: exception vector table, EL1 entry, BSS clear, early stack.
- `arch/arm64/uart.c`: PL011 UART driver (polled, no IRQ yet). Kernel `print()`.
- `arch/arm64/mmu.c`: identity map, enable MMU, map kernel VA range (TTBR1).
- `mm/phys.c`: physical frame allocator (buddy, simple). Parses DTB memory regions.
- `mm/vm.c`: kernel virtual memory map. `kmalloc` / `kfree` (slab-style, minimal).
- `arch/arm64/irq.c`: GIC v2/v3 init (autodetected from DTB). Exception vector dispatch.
  Spurious IRQ handling. Timer IRQ registered (no scheduler yet — just counts ticks).
- `lib/dtb.c`: minimal DTB parser. Extracts: memory regions, GIC base, UART base,
  timer IRQ number. No full FDT library needed.
- `kernel/main.c`: `boot_main()`. Calls subsystem inits in order. Prints boot banner.
  Hangs in a loop (no processes yet).

### 4.2 Exit criteria

- [ ] QEMU `virt` ARM64 boots to a UART message without crashing.
- [ ] `kmalloc`/`kfree` round-trip 10,000 allocations without leak (manual check).
- [ ] GIC initialized; timer IRQ fires at 1000 Hz (verified via UART counter).
- [ ] MMU on; kernel VA map correct (read/write kernel data, no fault).
- [ ] Sanitizer build (KASAN-lite equivalent or AddressSanitizer via instrumented build)
  runs without false positives on boot path.

### 4.3 Risks

- **DTB parsing**: QEMU's DTB is well-formed; real hardware is not. Risk is low for `virt`
  target. Mitigate: use a minimal hand-rolled parser; do not import a full libfdt.
- **GIC v2 vs v3**: QEMU `virt` defaults to GICv2 in older versions, GICv3 in newer.
  Support both; autodetect from DTB compatible string.

---

## 5. Phase 2: Process model + scheduler

**Goal**: the kernel can create, schedule, and destroy processes. `rfork`, `exec`,
`exits`, `wait` work. Two processes can run concurrently on a single CPU.

### 5.1 Deliverables

- `kernel/proc.c`: `Proc` struct. `rfork(RFPROC)` creates a new process, clones address
  space (no COW yet — just a new page table with shared mappings). `exits()` terminates.
  `wait()` reaps children.
- `kernel/sched.c`: preemptive round-robin scheduler. Per-CPU run queue. Timer IRQ drives
  preemption. `sched()`, `ready()`, `sleep()`, `wakeup()` — the Plan 9 scheduler idiom.
- `kernel/namespace.c`: `Pgrp` (process group / namespace). `bind()`, `mount()` (stub:
  only ramfs mountable at this phase), `unmount()`. Namespace cloned on `rfork(RFPROC)`;
  shared on `rfork(RFPROC|RFNAMEG)`.
- `arch/arm64/context.c`: `savectx()`, `restorectx()`. ARM64 register save/restore.
  Thread switch via `swtch()`.
- `kernel/exec.c`: `exec()`. Loads a static ELF binary from ramfs into the process
  address space. Jumps to entry point at EL0.
- `mm/vm.c` (extend): user address space management. `mmap` / `munmap`. Page fault
  handler: allocate-on-demand.
- `kernel/handle.c`: kernel object handle table. `KObj_Process`, `KObj_Thread`,
  `KObj_Chan` types at this phase. Handle allocation, rights checking, close.
  The typed handle infrastructure is established here; hardware handle types
  (MMIO, IRQ, DMA) are added in Phase 3 when drivers appear.
- `kernel/vmo.c`: VMO manager. `vmo_create()`, `vmo_create_physical()`,
  `mmap_handle()` for VMO mapping. Anonymous VMOs only at this phase; physical
  VMOs added in Phase 3.
- `kernel/pipe.c`: kernel pipe implementation. `pipe()` syscall. Blocking read/write
  with a ring buffer. Used by the debug shell.
- `init/init.c`: the first userspace process. Starts a minimal debug shell over UART.

### 5.2 Exit criteria

- [ ] Two processes run concurrently on a single CPU; timer preemption works.
- [ ] `rfork(RFPROC)` + `exits()` + `wait()` lifecycle works without leak.
- [ ] `exec()` loads and runs a static ELF from ramfs.
- [ ] `init` starts a UART shell; `echo hello` works via pipe.
- [ ] Page fault handler allocates demand pages; stack growth works.
- [ ] Handle table: open/close 10,000 handles without leak; rights reduction enforced.
- [ ] VMO: create, map, write, read, unmap, close cycle correct; pages freed on close.
- [ ] Stress: 1000 `rfork`/`exits`/`wait` cycles without leak or panic.

### 5.3 Risks

- **Scheduler correctness**: race conditions in `sched()` / `wakeup()` are subtle.
  Mitigate: implement and TLA+-specify the scheduler state machine before coding.
  Run with KASAN + lock validator from day one.
- **ELF loading**: static ARM64 ELF with `PT_LOAD` segments. Keep it simple: no dynamic
  linker, no interpreter. Dynamic linking is a Phase 5 concern.

---

## 6. Phase 3: Device model + VirtIO

**Goal**: the Dev vtable is implemented. VirtIO block and network devices work.
Disk I/O is possible. Network is possible (even if the stack is a stub).

### 6.1 Deliverables

- `kernel/dev.c`: Dev vtable infrastructure. `devtab[]` registration. Chan lifecycle.
  `walk()`, `open()`, `read()`, `write()`, `close()` dispatch.
- `kernel/handle.c` (extend): `KObj_MMIO`, `KObj_IRQ`, `KObj_DMA`, `KObj_Interrupt`
  handle types. `mmap_handle()` for MMIO regions. `irq_wait()` for interrupt delivery
  to userspace. `vmo_create_physical()` for CMA-backed DMA buffers.
- `kernel/irqfwd.c`: IRQ forwarding. Hardware interrupt arrives at EL1; kernel
  records delivery and wakes any process blocked on the corresponding `KObj_IRQ`
  handle via `irq_wait()`. Zero kernel involvement in the driver's hot path after
  initial setup.
- `dev/cons.c`: console device (`/dev/cons`, `/dev/consctl`). Wires UART to the Dev
  interface. Keyboard input via VirtIO input (if available) or UART.
- `dev/null.c`, `dev/zero.c`, `dev/random.c`: trivial kernel devices. ARM64 `RNDR`
  instruction for `/dev/random`.
- `kernel/virtio.c`: VirtIO core. Split virtqueue. MMIO transport. PCI transport (minimal,
  for VirtIO-GPU later). IRQ hookup via GIC.
- `drivers/virtio-blk/`: VirtIO block driver. Either:
  - **In-kernel** (fast path for Phase 3; promotes to userspace in Phase 6+), or
  - **Userspace 9P server** from day one (correct model; slower to implement).
  Decision: in-kernel for Phase 3, refactor to userspace in Phase 6.
- `drivers/virtio-net/`: VirtIO net driver. Exposes raw Ethernet frames via `/dev/ether0`.
  No TCP/IP stack yet — that's Phase 7.

### 6.2 Exit criteria

- [ ] `cat /dev/random` produces non-zero bytes.
- [ ] VirtIO block device readable: `dd if=/dev/vda of=/dev/null bs=4096 count=1024`
  completes without error.
- [ ] VirtIO block device writable: write a block, read it back, verify contents.
- [ ] Chan lifecycle: 10,000 open/read/close cycles on `/dev/null` without leak.
- [ ] Dev vtable: all 11 ops dispatch correctly for cons, null, zero, random.

### 6.3 Risks

- **VirtIO PCI**: needed for VirtIO-GPU. PCI enumeration on `virt` is simple (one root
  complex, linear BAR layout) but still needs careful implementation.
- **IRQ sharing**: VirtIO devices share IRQ lines in some QEMU configurations.
  GIC shared IRQ dispatch must be correct.

---

## 7. Phase 4: Filesystem + Stratum integration

**Goal**: Stratum runs as a userspace daemon. The kernel mounts it at `/`. The system
boots from Stratum. `janus` runs as a userspace 9P server.

### 7.1 Deliverables

- `kernel/9p.c`: 9P client. Implements the 9P2000.L wire protocol (or 9P2000 — decision
  pending based on Stratum's server dialect). Mounts a 9P server fd as a namespace entry.
  Translates Dev vtable calls to 9P messages. **Pipelined from day one**: maintains
  a per-session outstanding request table keyed by tag; up to 32 concurrent requests
  in flight per session. Out-of-order completion handled correctly.
- `fs/ramfs.c`: in-kernel ramfs Dev implementation. Holds initramfs contents. Unmounted
  after Stratum is running.
- `init/init.c` (extend): after Phase 2's UART shell, `init` now:
  1. Mounts ramfs at `/`.
  2. Starts `stratum` daemon (reads from VirtIO block).
  3. Starts `janus` daemon (key agent).
  4. Remounts `/` from Stratum.
  5. Starts `halcyon` (Phase 6) or the UART debug shell (Phase 4).
- Integration with **Stratum v2** (Phase 5 of Stratum roadmap): the kernel's 9P client
  speaks to Stratum's 9P server. Per-connection namespace isolation from Stratum §8
  means each process's kernel connection gets a private namespace view.

### 7.2 Exit criteria

- [ ] `stratum` daemon starts from initramfs, mounts a Stratum volume on VirtIO block.
- [ ] Kernel mounts Stratum 9P server at `/`.
- [ ] `ls /`, `cat /etc/hostname`, `mkdir /tmp/test` work via Stratum.
- [ ] `janus` starts and successfully unwraps a dataset key (passphrase backend).
- [ ] Reboot: data written before reboot is present after reboot (Stratum crash safety).
- [ ] 9P session: 10,000 open/read/write/close cycles on a Stratum file without
  protocol error or leak.

### 7.3 Risks

- **9P client correctness**: fid management, walk chains, clunk on close — these are
  subtle. Stratum's server is strict. The 9P client gets an audit round.
- **Stratum readiness**: Phase 4 of Thylacine depends on Stratum being at least at
  Phase 5 (multi-device) or beyond on its own roadmap. Coordinate timelines.

---

## 8. Phase 5: Syscall surface + musl port

**Goal**: a useful set of POSIX programs runs natively on Thylacine. Shell programs,
coreutils, and a C compiler are the target. The musl port is the primary deliverable.

### 8.1 Deliverables

- **Complete syscall table** (`kernel/syscall.c`): all syscalls listed in
  `ARCHITECTURE.md §11.1`, plus the Linux translation shim for static ARM64 binaries.
- **musl libc port**: musl's platform-specific layer (`arch/aarch64/`) is adapted to
  emit Thylacine syscalls. musl builds and links against Thylacine.
- **Dynamic linker**: `ld-thylacine.so` (musl's dynamic linker, relinked for Thylacine).
  Programs linked against musl (dynamically) run.
- **Coreutils port**: BusyBox or a similar minimal coreutils suite compiled against
  musl for Thylacine. `ls`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `grep`, `sed`, `awk`,
  `find`, `sort`, `wc`, `head`, `tail` all work.
- **`rc` port**: Plan 9's `rc` shell compiled for Thylacine as the native shell.
  (Halcyon is the graphical shell; `rc` is available as the scriptable alternative.)
- **`bash` port**: bash compiled against musl for Thylacine. Bash compatibility target
  for Halcyon's interactive parser.

### 8.2 Exit criteria

- [ ] musl builds clean for Thylacine.
- [ ] BusyBox coreutils run: `ls /`, `cat /etc/hostname`, `echo hello | grep hello`.
- [ ] A shell pipeline with 5 stages works: `cat /etc/passwd | grep root | cut -d: -f1`.
- [ ] `bash` starts and runs a non-trivial script (the coreutils test suite).
- [ ] A C program compiled with musl runs: allocates memory, opens a file, reads it,
  exits cleanly.
- [ ] Linux static ARM64 binary runs via the syscall shim: `echo hello` from a
  pre-built Alpine Linux binary.

### 8.3 Risks

- **musl port effort**: musl is designed for portability but has not been ported to
  Thylacine. The syscall interface is close enough to Plan 9 + POSIX that the port is
  tractable but non-trivial. Estimate: 2–4 weeks of focused work.
- **`epoll` gap**: many programs use `epoll`. Thylacine needs a poll/select equivalent.
  Implement a `poll()` compat surface using the 9P server model before Phase 5 exits.

---

## 9. Phase 6: Halcyon

**Goal**: Halcyon is the primary user interface. Images render inline. The development
workflow moves from UART debug shell to Halcyon.

### 9.1 Deliverables

- **VirtIO-GPU userspace driver** (`drivers/virtio-gpu/`): Rust. Exposes:
  ```
  /dev/fb/ctl      ← write: "res <w> <h>", "flush"
  /dev/fb/image    ← write: ARGB32 pixel data
  /dev/fb/info     ← read: current resolution
  ```
  Driver holds a `KObj_VMO` (physical, CMA-backed) for the framebuffer. Passes VMO
  handle to Halcyon on connect. Halcyon maps it and writes pixels directly — no copy
  through the 9P session for pixel data. `flush` via `/dev/fb/ctl` triggers DMA.
- **VirtIO-GPU driver promoted to userspace**: the in-kernel VirtIO block driver from
  Phase 3 is refactored as a userspace 9P server, validating the driver model.
- **Halcyon** (`halcyon/`): Rust. Graphical scroll-buffer shell.
  - Text rendering: monospace font (Iosevka Term Extended), fontdue rasterizer.
  - Graphical regions: inline pixel buffers in the scroll stream.
  - Image display: `display image.png` renders a PNG inline in the scroll buffer.
  - Scrollback: history of N lines (text + graphical).
  - Command input: bash-compatible interactive parser (partial bash feature set at v1.0).
  - 9P mount: Halcyon can mount a 9P server from the command line.
- **Image display**: `display` command. Decodes PNG (via `png` crate). Writes pixels
  to a graphical region in the scroll buffer.
- **Video player** (`drivers/video/`): Rust 9P server. At Phase 6: software decode only
  (via `ffmpeg` bindings or a minimal H.264 decoder). Exposes `/dev/video/player/`
  interface. HW decode deferred to post-v1.0.

### 9.2 Exit criteria

- [ ] Halcyon starts on boot (replaces UART debug shell as primary interface).
- [ ] Text output renders correctly (Iosevka, correct metrics, scrollback works).
- [ ] `display thylacine.png` renders a PNG inline in the scroll buffer.
- [ ] Scroll buffer: image scrolls away naturally as new output is added.
- [ ] `ls`, `cat`, `grep`, pipes — all work inside Halcyon.
- [ ] VirtIO block userspace driver: all Phase 3 block I/O tests still pass.
- [ ] Video: `play video.mp4` plays a short video (software decode) in the scroll buffer,
  controlled via `/dev/video/player/ctl`.

### 9.3 Risks

- **Font rendering**: fontdue is capable but Iosevka's wide Unicode coverage (needed for
  any code that uses math symbols, box-drawing, etc.) must be validated. Fallback:
  a bitmap font as a development placeholder.
- **Scroll buffer model**: the inline graphical region model is novel; edge cases
  around resize, reflowing text around images, and history trimming need careful design.
  Do this design pass before writing the rendering code.
- **virgl 3D**: not needed for Phase 6. Plain framebuffer is sufficient.

---

## 10. Phase 7: Compatibility layer

**Goal**: a meaningful set of pre-built Linux ARM64 binaries runs on Thylacine without
recompilation. Container-as-namespace model works.

### 10.1 Deliverables

- **Linux syscall shim** (extend Phase 5 shim): cover the top 50 Linux ARM64 syscalls
  by frequency of use in real programs. Priority: `openat`, `close`, `read`, `write`,
  `mmap`, `munmap`, `brk`, `exit_group`, `futex`, `clock_gettime`, `getpid`,
  `gettid`, `set_tid_address`, `rt_sigaction`, `rt_sigprocmask`, `pread64`, `pwrite64`,
  `lseek`, `fstat`, `newfstatat`, `getdents64`, `pipe2`, `dup3`, `execve`, `wait4`,
  `clone`, `nanosleep`, `poll`.
- **Synthetic Linux filesystem servers**: 9P servers that expose:
  - `/proc/<pid>/maps`, `/proc/<pid>/status`, `/proc/<pid>/cmdline` (for `ps`, `top`).
  - `/sys/` (minimal; enough for `ldd`, dynamic linker).
  - `/dev/` Linux layout (tty, urandom, null, zero, fd/).
- **Container runner** (`thylacine-run`): a userspace tool that:
  1. Takes an OCI container image (or a directory root).
  2. Constructs a new process namespace with the container root and synthetic servers.
  3. Starts the container's init inside the namespace.
  This is the "flatpak / Steam Deck" model. No cgroups, no seccomp — namespace isolation
  is sufficient for Phase 7.
- **Network stack** (`net/`): userspace 9P server providing TCP/IP. Based on smoltcp
  (Rust) or a port of Plan 9's IP stack. Exposes `/net/tcp/`, `/net/udp/`.
  VirtIO-net driver promoted to userspace at this phase.

### 10.2 Exit criteria

- [ ] `curl` (pre-built Linux ARM64 static binary) runs and fetches a URL.
- [ ] `python3` (pre-built musl-static Linux ARM64 binary) starts a REPL.
- [ ] Container runner: an Alpine Linux container image starts and a shell runs inside it.
- [ ] Network: `ping 1.1.1.1` works from inside Thylacine (ICMP via VirtIO-net).
- [ ] `wget https://example.com` works from Halcyon.

### 10.3 Risks

- **`futex`**: the most complex Linux syscall to translate correctly. Many threading
  libraries depend on it. May need a partial kernel implementation rather than a shim.
- **glibc dynamic linker**: glibc assumes `/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1`.
  Providing this is complex. Best-effort; musl-linked and static binaries are the
  primary compat target.
- **Network stack**: smoltcp is capable but not a full Linux network stack. Programs
  that assume specific socket options or kernel network behavior may fail.

---

## 11. Phase 8: Hardening + v1.0

**Goal**: Thylacine v1.0 is reliable enough to use as a daily development environment
(inside a VM). Boot-to-Halcyon is stable. Stratum is the root filesystem. The system
survives a week of continuous use without kernel panic.

### 11.1 Deliverables

- **Comprehensive audit pass**: every audit-trigger surface (exception handling,
  namespace operations, credential checks, 9P client, VirtIO drivers) gets a
  dedicated soundness audit.
- **Fuzzer integration**: AFL++ or LibFuzzer for:
  - The 9P client (malformed server responses).
  - The Linux syscall shim (malformed syscall arguments).
  - The ELF loader (malformed binaries).
- **Benchmark suite**: boot time, context switch latency, VirtIO block throughput,
  9P round-trip latency, Halcyon frame rate.
- **Documentation**: `docs/HACKING.md` (how to build and run), `docs/SYSCALLS.md`
  (full syscall reference), `docs/DRIVERS.md` (how to write a userspace driver).
- **v1.0 tag**: git tag, release notes, QEMU disk image.

### 11.2 Exit criteria

- [ ] 72-hour QEMU run without kernel panic (automated, via CI).
- [ ] All audit-trigger surfaces have a completed audit round with no P0/P1 findings.
- [ ] Boot to Halcyon in < 5 seconds (QEMU `virt`, 4 vCPUs, 4 GiB RAM).
- [ ] VirtIO block throughput ≥ 80% of QEMU-reported device limit.
- [ ] 9P round-trip latency p99 < 1 ms (loopback Stratum mount).
- [ ] Zero known kernel panics in the issue tracker.

---

## 12. Post-v1.0 roadmap

### 12.1 v1.1 candidates

- **SMP**: multi-CPU scheduling (the scheduler was SMP-aware from the start;
  enabling it requires IPI infrastructure and per-CPU data discipline per §20).
- **VirtIO-GPU virgl**: 3D acceleration via virgl; OpenGL ES for Halcyon if needed.
- **HW video decode**: VirtIO video decode extension; expose via `/dev/video/` HW path.
- **In-kernel Stratum driver**: bypass 9P overhead for root filesystem operations
  (mirrors Stratum's own post-v2.0 kernel driver roadmap).
- **`io_uring` compat**: for Linux binaries that depend on it.
- **Raspberry Pi 5 bare metal**: EL2→EL1 drop, mailbox framebuffer driver
  (`arch/arm64/rpi5/`), RP1 Ethernet for network boot. GIC-400 and PL011 UART
  drivers transfer from QEMU unchanged. Estimated: one focused sprint after v1.0.

### 12.2 v1.2 candidates

- **Apple Silicon bare metal**: m1n1 bootloader, AIC interrupt controller,
  AGX GPU via Asahi's driver (LinuxKPI shim approach).
- **Sound** (VirtIO sound device).
- **USB** (VirtIO USB or bare metal via Asahi's USB stack).

### 12.3 v2.0 candidates (long horizon)

- **RISC-V port**: target first RVA23-compliant SBC with PCIe + NVMe (Milk-V
  Jupiter 2 or equivalent). Port is mechanical above `arch/`: swap GIC for PLIC,
  ARM CSRs for RISC-V CSRs, ARM generic timer for SBI timer. Namespace, scheduler,
  9P client, VMO, and handle code unchanged.
- **x86-64 port**.
- **Network filesystem**: Stratum over the network (9P is already network-capable).
- **Rust kernel components**: selected kernel subsystems rewritten in Rust where the
  safety argument is most valuable (9P client, ELF loader, handle table).
- **MAC / capability system**: mandatory access control beyond namespace isolation.
- **Multikernel SMP**: per-core kernel instances communicating via explicit 9P
  messages (Barrelfish model). Long-horizon research direction.

### 12.4 Ruled out

- Graphical windowing system (compositor, window manager, display server).
- Binary-perfect glibc compatibility.
- Distributed / clustered OS.
- Windows binary compatibility.
- Backward compatibility with Linux kernel modules.

---

## 13. Risk register

| # | Risk | Phase | Severity | Mitigation |
|---|---|---|---|---|
| 1 | Scheduler race conditions (SMP) | 2 | HIGH | TLA+ spec before coding; TSAN from day one; single-CPU first |
| 2 | 9P client correctness | 4 | HIGH | Dedicated audit round; strict server (Stratum) catches bugs early |
| 3 | musl port effort | 5 | MEDIUM | Plan 9's libc as reference; musl designed for portability |
| 4 | `futex` translation | 7 | MEDIUM | Partial kernel futex; or limit compat to single-threaded programs initially |
| 5 | VirtIO-GPU / framebuffer | 6 | MEDIUM | Bitmap font fallback; plain framebuffer first, virgl later |
| 6 | Stratum timeline dependency | 4 | MEDIUM | Coordinate; ramfs + tmpfs sufficient until Stratum is ready |
| 7 | GIC v2/v3 autodetection | 1 | LOW | DTB compatible string; test both QEMU configurations |
| 8 | ELF loader edge cases | 2 | LOW | Static binaries only initially; fuzz the loader in Phase 8 |
| 9 | glibc compat | 7 | LOW | Explicitly best-effort; musl is the compat target |

---

## 14. Summary

Thylacine OS is an 8-phase journey from kernel skeleton to v1.0 release:

1. **Phase 0 (DONE)**: VISION.md, ARCHITECTURE.md, ROADMAP.md.
2. **Phases 1–8**: kernel → processes → devices → storage → userspace → shell → compat → v1.0.

Key commitments:
- ARM64 / QEMU `virt` throughout.
- C99 kernel; Rust userspace.
- 9P as the universal protocol.
- Userspace drivers from Phase 3 onward.
- Stratum as the native filesystem from Phase 4.
- Halcyon as the primary interface from Phase 6.
- Audit loop throughout.

The ordering is correct. The ambition is bounded. The thylacine runs again.
