# Thylacine v1.0 Novel Angles — Scope and Sequencing

**Status**: Phase 0 draft, 2026-05-04. Concrete scope, "done" definition, dependencies, complexity, and risk per lead position.

## 1. Purpose

`VISION.md` committed Thylacine v1.0 to ten lead positions versus Plan 9, 9Front, Linux, Fuchsia/Zircon, seL4, Redox, MINIX 3, and the multikernel research direction (Helios, Barrelfish). `COMPARISON.md` showed where the matches, leads, and deliberate non-competitions sit. This document is where we draw the line: for every angle, we define

1. **Scope** — what's in, what's deferred, what's out.
2. **Done** — a testable definition of "this shipped."
3. **Dependencies** — architectural decisions and other angles this rests on.
4. **Complexity** — rough KLOC estimate and spec count.
5. **Risk** — mature technique vs research-grade.
6. **Sequence** — what phase, and what could be cut if schedule pressure comes.

The goal is to make `ARCHITECTURE.md` easier to write: each angle becomes a known quantity so the architecture can focus on foundational decisions (concurrency model, memory model, boot model) rather than re-deriving scope for every novel feature.

## 2. Summary matrix

| # | Angle | Risk | LOC estimate | Sequence |
|---|---|---|---|---|
| 1 | 9P as universal composition, totalized | Low | 6–8 KLOC C99 | Continuous, foundational |
| 2 | Userspace drivers via typed handles + VMO zero-copy | Medium | 7–10 KLOC C99 + 8–12 KLOC Rust | Phase 2-3 |
| 3 | Pipelined 9P client with out-of-order completion | Low-medium | 3–5 KLOC C99 | Phase 4 (foundational for 4+) |
| 4 | Halcyon: shell as the graphical environment | Medium-high | 8–12 KLOC Rust | Phase 8 (final) |
| 5 | Stratum as native FS with namespace coupling | Low | 4–6 KLOC C99 (kernel side) | Phase 4 |
| 6 | EEVDF scheduler on Plan 9-heritage kernel | Medium | 4–6 KLOC C99 | Phase 2 |
| 7 | SOTA security hardening from day one | Low-medium | 3–5 KLOC C99 (mostly compiler/linker config + targeted code) | Phase 1-2 |
| 8 | Formal verification cadence: nine TLA+ specs | Low-medium | 4–6 KLOC TLA+ | Continuous |
| 9 | Designed-not-implemented v2.0 contracts | Low | ~0 KLOC (specifications only) | Phase 0 |

**Total novel-angle code**: ~50-70 KLOC of C99 (kernel + compat) + ~16-24 KLOC of Rust (drivers + Halcyon) + ~4-6 KLOC of TLA+. Within the ~100-130 KLOC total budget for a complete v1.0.

If any angle blows past its estimate by 2×, it's a warning sign worth redesigning — operating systems don't tolerate runaway complexity well, and the verification budget assumes implementations stay close to spec.

---

## 3. Per-angle scope

### 3.1 Angle #1 — 9P as the universal composition mechanism, totalized

**Why it's novel**: Plan 9 had 9P everywhere except authentication, graphics, and some device interfaces — those subsystems escaped the model. 9Front carried the model forward without totalizing it. Linux uses 9P for virtfs and a few container surfaces but treats it as one of many protocols. Fuchsia and seL4 use capability-based IPC; 9P is absent. Redox uses a scheme-based system in spirit similar to 9P but not protocol-compatible. **No production OS has totalized 9P** — every kernel resource, every userspace service, every POSIX surface, every driver, the graphical shell, the storage substrate, the key agent, all served as 9P trees.

Thylacine's totalization:

- **Drivers as 9P servers** (covered by angle #2).
- **POSIX surfaces as 9P servers**: `/proc`, `/dev`, `/sys`, `/dev/pts`, `/run`, `/tmp` are 9P trees served by dedicated processes (or kernel-internal `Dev`s for performance-critical ones). Linux programs see what they expect; underneath it's all 9P.
- **The kernel's administrative interface** is `/ctl/` — a synthetic 9P tree exposing kernel configuration, scheduler stats, IRQ counts, namespace dumps, etc.
- **Halcyon mounts 9P servers** for everything it displays (framebuffer, video, fonts, etc.).
- **Stratum is a 9P server** mounted as the root FS.
- **janus is a 9P server** mounted as `/dev/janus/`.

**Scope — in**:
- Kernel `Dev` vtable + `Chan` + `Walkqid` + `attach` / `walk` / `open` / `read` / `write` / `clunk` / etc. as the unified resource interface.
- Kernel-internal `Dev` implementations: `cons`, `consctl`, `null`, `zero`, `random`, `proc`, `procfs`, `ctl`, `ramfs`.
- Synthetic 9P servers (userspace) for: Linux-compat `/proc`, Linux-compat `/sys`, `/dev/pts/`, `/run/`.
- Kernel 9P server-ification of itself: `/ctl/` is a kernel `Dev`.
- Bind / mount / unmount syscalls as the only namespace composition primitives.
- Union mounts via `bind(MBEFORE | MAFTER)`.

**Scope — deferred** (post-v1.0):
- 9P-over-network as a first-class composition primitive for distributed namespaces. 9P is network-capable today; we don't make multi-machine workflows a v1.0 design center.
- 9P RPC extensions for service mesh / RPC-style use cases. The 9P read/write model is sufficient for v1.0.

**Scope — out**:
- Replacing 9P with a different protocol "for performance" in any subsystem. The performance envelope is delivered by pipelining (angle #3), VMOs for zero-copy bulk data (angle #2), and the latency budget (VISION §4.5).
- A separate IPC mechanism alongside 9P. There is exactly one composition mechanism in Thylacine.

**Done definition**:
- Every resource exposed by the OS is reachable as a path in some process's namespace.
- Every operation against any resource is a 9P message (or a kernel-internal `Dev` call that mirrors 9P semantics).
- A shell user can browse `/proc/<pid>/`, `/dev/`, `/dev/fb/`, `/net/tcp/`, `/dev/video/player/`, `/dev/janus/keys/` with `ls`, `cat`, `echo` — no special tools.
- Adding a new "kernel feature" means writing a new 9P server, not modifying the kernel.
- A program written for Linux that opens `/proc/self/cmdline` reads the expected bytes via Thylacine's compat 9P server.
- **Utopia milestone (Phase 5 exit)**: a developer using Thylacine via SSH or UART console finds a complete textual POSIX environment that "feels real, not broken" (per VISION §13). Everything below works on the 9P substrate: shell (`rc` + `bash`), full coreutils (uutils-coreutils, complete flag coverage — not stripped), `vim`, `less`, `top`, `tmux`, `ssh`, `git`, `make`, `python3`, `gcc`/`clang`, all tools that depend on `termios` / `poll` / `pty` / signal handling / threading. BusyBox is the recovery shell in initramfs; coreutils is the daily driver. Utopia is the test that the angle's totalization is real, not just theoretical — if any POSIX surface is broken, Utopia exposes it before later phases (Linux compat at Phase 6, hardening at Phase 7, Halcyon at Phase 8).

**Dependencies**:
- Kernel `Dev` infrastructure (Phase 2-3).
- 9P client in kernel (Phase 4).
- Stratum 9P server up at Phase 4 (external dependency on Stratum Phase 9).
- Userspace 9P server framework (Phase 4-5; libraries for Rust + C).

**Complexity**:
- Kernel `Dev` framework: ~1.5 KLOC.
- Kernel-internal `Dev` implementations (cons, null, zero, random, proc, ctl, ramfs): ~3 KLOC across all.
- Userspace 9P server library (C99 + Rust bindings): ~1.5 KLOC.
- Synthetic Linux-compat servers (`/proc`-Linux, `/sys`-Linux, `/dev/pts`): ~1-2 KLOC each, ~3-4 KLOC total.
- Total: 6–8 KLOC C99.

**Risk — Low**:
- Plan 9 / 9Front have proven the model works.
- 9P2000.L is well-documented and stable.
- Stratum's 9P server is the integration target; Stratum already speaks the dialect we need.
- Risk is in totalization rigor — it's tempting to ad-hoc a small subsystem instead of writing a 9P interface for it. The audit policy (CLAUDE.md) catches this.

**Alternative approaches considered**:
- **Capability-based IPC** (Fuchsia / seL4 model). Rejected because: hierarchical composition is a first-order Plan 9 strength; capabilities don't compose hierarchically without a separate hierarchy layer. Capabilities also don't give you "discoverable via `ls`" — you have to know what capability you have. The shell-as-UI thesis depends on discoverability.
- **Microkernel RPC + custom protocol per subsystem** (MINIX 3 / Helios). Rejected because: this is exactly the fragmentation 9P is designed to eliminate. Each protocol is a new attack surface, a new client library, a new audit target.
- **Linux-style mixed protocols (sockets + sysfs + procfs + netlink + dbus + ...)**. Rejected because: the same fragmentation problem at higher cost.

**Sequence**: Foundational. Phase 1 (kernel skeleton) starts with `Dev` + `Chan` infrastructure. Phase 4 lands the 9P client. POSIX-compat 9P servers land at Phase 5-6. The model is in continuous refinement throughout v1.0; full totalization is verified at the Phase 7 audit pass (every kernel feature reachable as a 9P path) and re-validated at Phase 8 with Halcyon's additional 9P-server surface.

---

### 3.2 Angle #2 — Userspace drivers via typed kernel handles + VMO zero-copy

**Why it's novel**: Fuchsia / Zircon pioneered the typed-handle + VMO model. seL4 has equivalent capability-based machinery. MINIX 3 has userspace drivers without the zero-copy zero-IPC-overhead story. **No system has combined typed handles + VMOs with 9P as the public driver interface and the subordination invariant making the 9P-mediated transfer the only path.** This is Thylacine's contribution — not the typed-handle model alone (Fuchsia has it) but its subordination to 9P as the universal composition mechanism.

The model:

- Drivers are userspace processes. They hold typed kernel handles for hardware access:
  - `KObj_MMIO` — mapped MMIO region for the device's BAR.
  - `KObj_IRQ` — right to receive a specific interrupt.
  - `KObj_DMA` — DMA-capable physically contiguous buffer (CMA-allocated).
- These handles are **non-transferable by type**. The transfer syscall has no code path for them; attempting to transfer panics the offender. (Driver crashes are isolated; misbehaved-but-not-malicious driver bugs are not a hard kernel failure.)
- Drivers expose results via a 9P server. Bulk data (framebuffer pixels, decoded video frames, packet rings) is shared via VMO handles transferred over the 9P session.
- VMOs are first-class kernel objects: anonymous (zero-filled on demand), physical (pinned for DMA), or file-backed (Stratum page cache, post-v1.0). Reference-counted; pages live as long as any handle or mapping refers to them.
- VMO handles transfer between processes only via 9P sessions (subordination invariant — VISION I-4).

**Scope — in**:
- Kernel handle table per-process; eight handle types (`Process`, `Thread`, `VMO`, `MMIO`, `IRQ`, `DMA`, `Chan`, `Interrupt`).
- Right-bit model with monotonic reduction on transfer.
- Typed transfer syscall covering only `Transferable` handles (`Process`, `Thread`, `VMO`, `Chan`).
- VMO manager: anonymous + physical, page lifecycle, mapping into address spaces.
- IRQ forwarding kernel infrastructure: hardware IRQ → kernel records → wakes the IRQ-handle blocker; zero kernel involvement after handle setup.
- VirtIO core in kernel (transport-level only): virtqueue management, descriptor chains, IRQ hookup. Devices are userspace.
- Userspace virtio-blk, virtio-net, virtio-input, virtio-gpu drivers (Rust) at Phase 3.
- `vmo_create` / `vmo_create_physical` / `mmap_handle` / `irq_wait` syscalls.

**Scope — deferred** (post-v1.0):
- IOMMU-mediated DMA isolation (ARM64 SMMU). v1.0 trusts driver processes; v2.0 adds IOMMU enforcement so driver bugs can't DMA over kernel memory.
- File-backed VMOs (Stratum page cache integration). v1.0 has anonymous + physical only.
- Hardware video decoder driver (`KObj_DMA`-fed decoder). v1.0 is software decode in the video player.

**Scope — out**:
- General-purpose handle transfer outside 9P sessions. The subordination invariant is non-negotiable.
- Transferable hardware handles. `KObj_MMIO`, `KObj_IRQ`, `KObj_DMA` are fundamentally untransferable.
- Capability-based replacement of namespaces. Handles are private to drivers; namespaces are public.

**Done definition**:
- Phase 3 ships userspace virtio-blk, virtio-net, virtio-input as 9P servers, with all hardware access via typed handles. No in-kernel VirtIO device driver code at v1.0.
- `KObj_MMIO`, `KObj_IRQ`, `KObj_DMA` cannot be transferred — verified by a runtime test that attempts `transfer(KObj_MMIO_handle)` and asserts a kernel-detected violation.
- A VirtIO-GPU framebuffer write completes without copy: driver creates VMO, Halcyon receives handle via 9P, maps it, writes pixels, signals via `/dev/fb/ctl flush`. Verified by performance regression: zero-copy framebuffer write achieves > 99% of memcpy-bandwidth limit.
- Driver crash test: kill the virtio-blk driver mid-I/O. Kernel does not crash. Filesystem mount remains; subsequent mount-restart reconnects.
- Specs `handles.tla` and `vmo.tla` clean under TLC.

**Dependencies**:
- Phase 2 produces the handle table + VMO manager + IRQ forwarding infrastructure.
- Phase 3 produces the VirtIO core + userspace drivers.
- 9P client (Phase 4) for cross-process VMO handle transfer.

**Complexity**:
- Kernel handle table: ~1.5 KLOC (alloc, close, rights check, transfer-syscall typed dispatch).
- VMO manager: ~2 KLOC (anonymous + physical paths, mapping, refcount, free).
- IRQ forwarding: ~1 KLOC (GIC-to-userspace dispatch, blocker wake).
- VirtIO core: ~2 KLOC (virtqueue, descriptor chain, IRQ hookup).
- Userspace virtio-blk driver: ~2 KLOC Rust.
- Userspace virtio-net driver: ~2 KLOC Rust.
- Userspace virtio-input driver: ~1 KLOC Rust.
- Userspace virtio-gpu driver: ~3 KLOC Rust (framebuffer + virgl 2D).
- Total: 7–10 KLOC C99 (kernel side) + 8–12 KLOC Rust (drivers).

**Risk — Medium**:
- Typed-handle model has prior production art (Fuchsia, seL4). Risk is correctness of our specific subordination invariant and the zero-copy path's lifetime correctness. `handles.tla` and `vmo.tla` cover these.
- Userspace IRQ-to-handler latency must hit the < 5µs p99 budget. ARM64 exception path is clean; ~1-2µs is achievable on Apple Silicon. If we miss this number, the entire driver model is performance-suspect.
- Driver process crash recovery: when a driver crashes, all its 9P sessions clunk; the kernel must reclaim its handles cleanly. VMOs must remain alive for any other process holding their handles. This lifetime story is what `vmo.tla` proves.

**Alternative approaches considered**:
- **In-kernel drivers** (Linux model). Rejected: monolithic kernel; driver bugs crash kernel; updates require module reload. Phase 3 with in-kernel virtio-blk was the priming's expedience shortcut; rejected under SOTA tenet.
- **FUSE-style userspace drivers** (Linux's existing mechanism). Rejected: FUSE has known performance limits; not a clean primitive.
- **Capability-based IPC for drivers** (Fuchsia model without 9P). Rejected: drivers expose 9P interfaces as the public contract; capabilities are private mechanisms.
- **Process-per-driver-per-device** vs **one driver process for many devices**. Resolved: one driver process per *device class* (one virtio-blk process serving all virtio-blk devices), not per-device. Driver implements per-device state internally.

**Sequence**: Phase 2 (handle infrastructure + VMO manager) → Phase 3 (VirtIO core + userspace drivers). The Phase 2 work is foundational; Phase 3 is the first live test of the model.

---

### 3.3 Angle #3 — Pipelined 9P client with out-of-order completion

**Why it's novel**: 9P2000 already supports pipelining at the protocol level — the `tag` field exists for exactly this purpose. **Plan 9's reference 9P client and most 9P clients in the wild do not exploit it** — they serialize requests, sending a Tmessage and waiting for the matching Rmessage before sending the next. This wastes the protocol's potential. Stratum's server (per Stratum's NOVEL angle #7) already supports pipelined I/O via io_uring. Linux's `v9fs` client serializes. Plan 9's `mount` driver mostly serializes (with edge cases). Thylacine's kernel 9P client pipelines from day one.

The model:

- Per-session outstanding-request table indexed by 9P tag.
- Up to 32 (configurable) concurrent in-flight requests per session.
- Tags allocated from a per-session pool; freed on Rmessage receipt.
- Out-of-order completion: tag N's Rmessage wakes tag N's waiter regardless of issue order.
- Kernel threads parking on a 9P request hold their `tag` and a wait-queue entry; the receive loop matches incoming Rmessages to tags and wakes the waiter.
- Flow control: per-session credit-based limit on outstanding requests; new requests block if at limit until a slot frees. Per-session config exposed via `/ctl/9p/<session>/max-outstanding`.
- Userspace 9P servers benefit automatically — every server already handles multiple tags (Stratum does; the Plan 9 `lib9p` does).

**Scope — in**:
- Per-session tag allocator (16-bit tag space, tag 0xFFFF reserved for Tversion).
- Per-session outstanding-request table.
- Pipelined send (issue without wait).
- Out-of-order Rmessage dispatch.
- Bounded outstanding-request limit; flow control.
- Pipeline state observable at `/ctl/9p/<session>/{outstanding,tags-in-use,issued-total}`.
- Halcyon issues concurrent reads against multiple 9P servers (keyboard, framebuffer, video frame, command pipe) via the pipelining client — no separate async I/O syscall needed for this case.

**Scope — deferred** (post-v1.0):
- 9P session multiplexing across multiple kernel threads (single receive loop per session at v1.0).
- Tag-space exhaustion recovery via dynamic tag reuse with sequence numbers (16-bit tag space is sufficient for v1.0 workloads at 32 outstanding/session; tag aliasing protected by per-session monotonic generation if it becomes a concern).

**Scope — out**:
- Completely lock-free 9P client. v1.0 uses a per-session lock for the outstanding-request table; the lock is held briefly (insert + lookup) and is not on the data path.

**Done definition**:
- Spec `9p_client.tla` proves: tag uniqueness per session, no Rmessage routed to wrong waiter, no missed wakeups, bounded staleness of pending tags.
- Throughput test: process issuing 32 concurrent reads on a Stratum mount achieves throughput ≥ 90% of the session's bandwidth limit (vs ~3% for naive serialized 9P at typical RTT).
- 9P round-trip p99 (loopback): < 500µs (VISION §4.5 budget).
- Stress test: 1000 concurrent 9P operations across 10 sessions for 1 hour; no leaks, no protocol errors, no missed wakeups.
- Halcyon test: keyboard input, video frame poll, command-output read all in flight simultaneously without a dedicated async syscall.

**Dependencies**:
- 9P2000.L wire protocol implementation.
- Kernel wait-queue infrastructure (Phase 2).
- 9P client (Phase 4).

**Complexity**:
- Tag allocator: ~200 LOC.
- Outstanding-request table: ~400 LOC.
- Pipelined send + dispatch loop: ~1 KLOC.
- Flow control: ~300 LOC.
- 9P2000.L message encode/decode: ~1.5 KLOC.
- Stratum extension messages (Tbind, Tunbind, Tpin, Tunpin, Tsync, Treflink, Tfallocate): ~500 LOC.
- Total: 3–5 KLOC C99.

**Risk — Low-medium**:
- 9P pipelining is part of the protocol; not a research extension.
- Stratum already supports it; integration is straightforward.
- Risk is correctness of the wait/wake state machine — `9p_client.tla` covers it.
- Risk is missed-wakeup races between blocking thread and dispatch loop — same TLA+ spec covers it.

**Alternative approaches considered**:
- **Naive serialized 9P** (Plan 9 reference, Linux v9fs). Rejected: throughput cliff at typical RTT.
- **Async-only 9P with no blocking syscall surface** (would force every read to be `read_async + poll`). Rejected: breaks the "blocking syscalls compose naturally" guarantee. Halcyon issues N concurrent blocking reads via pipelining; the scheduler parks each on its tag.
- **io_uring-style submission/completion queue ABI in Thylacine**. Deferred to post-v1.0. The pipelining client gives us most of io_uring's win for the 9P case; io_uring's value is for the syscall layer where we don't have a kernel boundary like 9P.

**Sequence**: Phase 4 (9P client). Foundational for Phase 4-8 — every subsequent phase depends on the 9P client being fast.

---

### 3.4 Angle #4 — Halcyon: shell as the graphical environment

**Why it's novel**: every modern OS uses some compositor / window manager / display server (Wayland, X11, Scenic, AppKit, DWM). Plan 9's Rio + 8½ is the closest comparison — a tile-based windowing system without compositing — but it's still a windowing system, with its own protocol and event loop. **No production OS uses a scroll buffer with inline graphics as the sole UI primitive.** Smalltalk environments and some Lisp machines came close in the 1980s but never with modern hardware acceleration. Notebook UIs (Jupyter, Mathematica) achieve something similar but inside another OS's windowing system.

Halcyon is the bet that, for shell-driven development workflows, this is enough.

**Halcyon is deliberately the last phase of v1.0.** It presupposes Utopia (Phase 5 exit), Linux compat + network (Phase 6 exit), and full hardening + audit (Phase 7 exit). Halcyon arrives at Phase 8 *on top of* a hardened, audited, Linux-binary-compatible, network-capable substrate, adding inline graphics, image display, and video playback to an already-working environment. This sequencing is load-bearing for two reasons:

1. **Risk isolation**: Halcyon is the highest-risk angle (medium-high; the scroll-buffer-with-inline-graphics model is novel in production). Holding it to last means its risk does not endanger the rest of the OS. If Halcyon's design has surprising edge cases, the textual + compat substrate is the user's working environment until Halcyon stabilizes.

2. **Shippable v1.0-rc fallback**: Phase 7 exit produces a complete, hardened, audited, Linux-compatible textual OS — a real shippable v1.0-rc. If Halcyon hits a wall at Phase 8, the project ships the v1.0-rc as v1.0 and treats Halcyon as v1.1. The "practical working OS" the user committed to (per VISION §13) is delivered regardless.

Halcyon is *additive* over the practical OS; it is not a replacement. Thylacine never enters a state where the user has nothing usable.

The model:

- Halcyon is a userspace Rust program. It mounts `/dev/fb/` (the framebuffer 9P server), `/dev/cons` (keyboard), and any other 9P servers it needs.
- The display is a scroll buffer. Each entry is either:
  - A **text region**: monospace text rendered with current attributes (color, weight, etc.).
  - A **graphical region**: pixel-addressable bitmap (image, video frame, plot).
- Output appends to the buffer; old entries scroll off the top (configurable history depth).
- Commands are entered at the bottom; output renders inline.
- Image display: `display image.png` adds a graphical region, sized as the image dimensions.
- Video: a 9P server (`/dev/video/player/`) decodes; Halcyon polls `frame` (which returns a VMO handle to the decoded frame buffer); blits to a graphical region; updates as `frame` content changes.
- No overlapping regions, no resizable windows, no z-order. Time-ordered scroll.

The Rust implementation matters: parsing bash-subset syntax, managing the scroll buffer state machine, decoding PNGs, rendering fonts via fontdue — these are exactly the domains where C UAF / overflow CVEs have been historically prolific. Rust's borrow checker eliminates the class.

**Scope — in**:
- Scroll-buffer rendering: text regions + graphical regions in time order.
- Monospace text rendering via fontdue (configurable font; Iosevka or equivalent at v1.0).
- ANSI escape sequence support (colors, cursor moves) for compatibility with programs like `vim`, `less`, `top`.
- Image display: PNG, JPEG, and stretch-to-include WebP. Renders inline, scrolls naturally.
- Video playback via `/dev/video/player/` — software H.264 decode at v1.0.
- Bash-subset interactive parser: `cmd args`, `|`, `>`, `<`, `&`, `&&`, `||`, simple `$VAR` substitution. Job control (`Ctrl-Z` / `bg` / `fg`) at v1.0. Full bash compatibility is post-v1.0; programs that need bash-specific features run `bash` as a subshell.
- 9P mount commands: `mount -t 9p server path`.
- VMO handle reception: video frame buffers transferred zero-copy via 9P session.

**Scope — deferred** (post-v1.0):
- Hardware video decode via VirtIO video extension.
- Color-managed image display.
- Inline plot rendering (matplotlib-style — would need a plotting library).
- Bash compatibility beyond the subset.
- Configurable themes / color schemes (v1.0 has one default theme).

**Scope — out**:
- Overlapping windows of any kind.
- Side-by-side panes (no `tmux`-style splits in Halcyon proper; `tmux` runs inside Halcyon as a normal program with no special integration).
- Pop-up dialogs, modal prompts, transient panels. (Password prompts are line-oriented reads from `janus`; confirmation is `[Y/n]`.)
- Mouse-driven UI affordances (Halcyon supports mouse for selection / scrolling, not as a primary input model).
- Multi-monitor.
- Web browser, multi-pane IDE, Office suite — applications that fundamentally require windowing.

**Done definition**:
- Halcyon starts on boot; replaces UART debug shell as primary interface.
- Phase 6 exit criteria: text rendering correct (Iosevka or equivalent, correct metrics, scrollback works); `display thylacine.png` renders inline; `play video.mp4` plays in scroll buffer; `ls`, `cat`, `grep`, pipes all work.
- Frame time p99 < 16ms (60Hz floor — VISION §4.5 budget).
- Memory: Halcyon resident size < 64 MiB with 100k-line scrollback at typical text density.
- A user runs `vim` inside Halcyon and edits a Rust source file with syntax highlighting.
- A user runs `tmux` inside Halcyon and gets multi-pane workflows (via tmux's own model — Halcyon doesn't know about panes).

**Dependencies**:
- VirtIO-GPU userspace driver (`/dev/fb/`) — Phase 3 (driver) + Phase 8 (Halcyon-side).
- Video decoder (software at v1.0) as 9P server — Phase 6.
- `pthread`, `futex`, `poll`, `pty`, `termios` — Phase 5.
- `bash` port — Phase 5.
- VMO handle transfer over 9P — Phase 4.

**Complexity**:
- Scroll buffer model + rendering: ~3 KLOC Rust.
- Font rendering (fontdue wrapper): ~500 LOC Rust.
- Image decoding (`png`, `image` crates): ~500 LOC Rust.
- Bash-subset parser: ~2 KLOC Rust.
- Job control: ~500 LOC Rust.
- ANSI escape handling: ~1 KLOC Rust.
- Video player 9P client + frame compositor: ~1 KLOC Rust.
- 9P mount + general mounting: ~500 LOC Rust.
- Total: 8–12 KLOC Rust.

**Risk — Medium-high**:
- The scroll-buffer-with-inline-graphics model is novel in production. Edge cases around resize, reflow, history trimming, mixed text/graphics scrolling need careful design before the rendering code is written.
- "Shell-driven workflows is enough" is a market hypothesis. We commit to it as a design choice, but if real users find Halcyon untenable, the v1.0 release suffers.
- Font rendering quality is the visible quality bar — Iosevka through fontdue must look as good as `vim` in `iTerm2`. If fontdue produces lower-quality output than alternatives, we evaluate a fallback (RustyType, swash) but Iosevka must work.

**Alternative approaches considered**:
- **Lightweight tiling window manager** (dwm-style on Plan 9). Rejected: still a windowing system, still a separate composition mechanism alongside 9P. Halcyon's "everything is a 9P client" posture is part of the thesis.
- **Notebook-style UI** (Jupyter kernel + cells). Rejected: notebooks assume a host browser; Halcyon is the OS UI. Cells can be a Halcyon-internal feature post-v1.0 if we want them.
- **Plain text terminal + sixel-like inline graphics** (modern terminal emulators). Considered. Sixel works inside `iTerm2`, etc. But Halcyon goes further: graphical regions are first-class scroll-buffer entries with structured history, not encoded-bytes-in-text. This makes the implementation cleaner (no escape-sequence soup) and the UX richer (scrolling preserves images byte-for-byte).
- **Wayland compositor with everything routed through it** (modernist alternative). Rejected: this is the standard; we are deliberately the not-standard.

**Sequence**: Phase 8 (the final phase of v1.0). Halcyon is the v1.0 marquee feature; it's also the riskiest, and lands after the entire dependency chain (drivers, 9P, Stratum, Utopia, Linux compat, network, hardening, audit) is solid. Phase 7 exits with a hardened, audited textual v1.0-rc that is shippable as v1.0 if Halcyon slips. This is deliberate insurance.

---

### 3.5 Angle #5 — Stratum as native filesystem with namespace coupling

**Why it's novel**: most OSes have *a* filesystem — usually whatever the kernel ships (ext4, NTFS, APFS, Minfs). Plan 9 had cwfs / fossil / kfs — adequate but not state-of-the-art. **No OS has a filesystem as advanced as Stratum (PQ-encrypted, formally verified, Merkle-rooted, content-defined chunking, lock-free metadata, succinct in-RAM state) AND has the OS's namespace model coupled to the filesystem's namespace model AND has the OS-FS interface be exactly the protocol the FS already speaks**. Thylacine on Stratum is a unique combination because Stratum was independently designed to be 9P-native; the coupling is a free lunch.

The model:

- Stratum runs as a userspace daemon. The kernel mounts it at `/` via `mount(stratum_fd, "/", ...)` after the initramfs phase.
- The kernel's 9P client speaks 9P2000.L + Stratum extensions (Tbind, Tunbind, Tpin, Tunpin, Tsync, Treflink, Tfallocate) to Stratum.
- Each Thylacine process gets its own 9P connection to Stratum (one connection per Proc; at v1.0 — see VISION §11 for the rationale).
- Per-connection Stratum namespace = per-process Thylacine namespace inside Stratum's purview. Composition within Stratum (multiple subvolumes overlaid) uses Stratum's `Tbind` / `Tunbind`. Composition across servers (Stratum + a network FS) uses Thylacine's `bind` / `mount` (kernel-level).
- Stratum's per-extent encryption, Merkle integrity, lock-free metadata, snapshots, clones, send/recv, dedup — all transparent to the kernel. The kernel sees: 9P operations succeed or fail; when they succeed, the data is integrity-verified.
- Stratum's `janus` key agent runs as another userspace 9P server (`/dev/janus/`). Halcyon, programs needing key-mediated operations, interact with janus over 9P.

**Scope — in**:
- Kernel 9P client speaking 9P2000.L + Stratum extensions (Phase 4).
- `mount` syscall integration: `mount(fd, path, "subvol_name")` translates to `Tattach` with the subvolume name as `aname`.
- Per-process 9P connection establishment at process creation (`rfork`).
- Connection cleanup at process termination.
- Initramfs → Stratum boot transition: ramfs holds `stratum`, `janus`, `init` binaries; init starts `stratum` + `janus`; kernel remounts root from Stratum.
- Stratum's authentication via janus 9P session at mount time.

**Scope — deferred** (post-v1.0):
- In-kernel Stratum driver bypassing 9P client overhead (designed in `ARCHITECTURE.md §14.4`; implemented post-v1.0).
- Stratum subvolume composition across multiple Stratum pools (today: one pool per kernel mount; multi-pool composition is a v2.x feature).

**Scope — out**:
- Stratum-specific kernel APIs that bypass 9P. The discipline: Thylacine talks to Stratum exactly as any 9P client would. If we need to bypass 9P for performance, we add a 9P extension or build the in-kernel driver — not a Stratum-specific syscall.

**Done definition**:
- Phase 4: kernel mounts Stratum at `/`; `ls`, `cat`, `mkdir`, `rm`, `cp` all work.
- Reboot test: data written before reboot is present after reboot, integrity-verified by Stratum's Merkle layer.
- Per-process namespace test: process A binds `/home/alice` from one subvolume, process B binds `/home/alice` from another; both succeed without interference.
- janus integration: a passphrase-protected dataset can be unwrapped via janus and become readable.
- 9P round-trip latency (Stratum loopback) p99 < 500µs.
- Stress: 1000 concurrent file operations across 10 connections without leak or protocol error.
- Encryption / snapshot / clone / send-recv operations tested at the Stratum-CLI level inside Thylacine.

**Dependencies**:
- Stratum at Phase 9 (9P server + extensions). External dependency, coordinated.
- 9P client (this is angle #3).
- Kernel's `mount` syscall + initramfs phase (Phase 4).

**Complexity** (kernel side; Stratum is independent):
- 9P2000.L wire protocol: ~1.5 KLOC C99 (covered in angle #3 estimate; not double-counted).
- Stratum extension messages: ~500 LOC C99 (covered in angle #3 estimate).
- Per-process connection management: ~1 KLOC C99.
- Initramfs → Stratum transition logic: ~500 LOC C99.
- Authentication via janus: ~300 LOC C99.
- janus client library: ~500 LOC C99.
- Mount-syscall integration: ~500 LOC C99.
- Total: 4–6 KLOC C99 (kernel side, beyond the 9P client itself).

**Risk — Low**:
- Stratum is feature-complete. Phase 9 (9P server) is its next milestone; coordination is straightforward.
- 9P2000.L + Stratum extensions are documented.
- per-connection isolation is Stratum's responsibility; Thylacine just establishes connections.
- The boot-from-Stratum sequence has known complexity; mitigated by initramfs fallback.

**Alternative approaches considered**:
- **In-kernel Stratum driver from v1.0**. Rejected: ~10-20 KLOC of new kernel code at v1.0 entry; high audit cost; the 9P-client path is fast enough. v2.0 hook designed.
- **Stratum library linked into kernel** (single-binary kernel + FS). Rejected: violates the 9P-as-IPC discipline; entire Thylacine architecture is 9P-mediated.
- **A different FS at v1.0** (start with ext4 or a simple FS, switch to Stratum later). Rejected: the architectural alignment is the entire point; using anything else is a backward step.

**Sequence**: Phase 4. Lines up with Stratum Phase 9. The two phases coordinate; Thylacine Phases 1-3 proceed in parallel with Stratum's Phase 8 work.

---

### 3.6 Angle #6 — EEVDF scheduler on Plan 9-heritage kernel

**Why it's novel**: Plan 9 uses a simple priority-based preemptive scheduler with priority decay. Linux uses EEVDF (since 6.6, replacing CFS). Fuchsia uses a custom fair-share scheduler. seL4 has multiple plug-in schedulers, often round-robin within priority. **No Plan 9-heritage kernel uses a modern fair-share algorithm with provable latency bounds.** Thylacine's combination — Plan 9 process model + `rfork` + notes + per-CPU run queues + EEVDF — is novel.

The model:

- **Earliest Eligible Virtual Deadline First** (Stoica et al 1995, refined since). Each runnable thread has a virtual eligible time and a virtual deadline; the scheduler picks the thread with the earliest virtual deadline among eligible threads.
- Provable properties: bounded service-time variance, latency proportional to weight, fair under all weights.
- Three priority bands as separate run-trees: interactive (high weight, short deadlines), normal (default), idle (low weight).
- **Per-CPU run trees**, not global. Work-stealing on idle: an idle CPU steals from the busiest peer.
- ARM generic timer for tick (or tickless: program the next deadline; sleep until then).
- Preemption at EL0→EL1 boundary in Phase 2 (syscall + IRQ entry); kernel preemption deferred to Phase 7 hardening.
- Single-CPU baseline at Phase 2 entry; SMP enabled at Phase 2 exit (4 CPUs first; up to 8 by v1.0).

**Scope — in**:
- EEVDF data structures per band per CPU.
- Eligibility / deadline computation.
- Run-tree (red-black tree or skip list) for ordered scheduling.
- Work-stealing on idle.
- IPI-driven cross-CPU wakeup.
- Tickless idle.
- Per-CPU scheduler stats exposed at `/ctl/sched/cpu<N>/`.
- Plan 9 idioms preserved: `sched()`, `ready()`, `sleep()`, `wakeup()`, `rendezvous()`.
- Spec `scheduler.tla` proves: progress (every runnable thread eventually runs), latency bound (proportional to weight), wakeup atomicity (no missed wakeups across wait/wake races), IPI ordering (cross-CPU operations don't lose state).

**Scope — deferred** (post-v1.0):
- Hard real-time scheduling class (FIFO / RR with priority inheritance). v1.0 has soft latency bounds via EEVDF.
- CPU isolation / dedicated cores (cpuset-equivalent). v1.0 schedules everywhere.
- Energy-aware scheduling (big.LITTLE on ARM). v1.0 treats all cores as equal.
- Multikernel direction (per-core kernel instances). Designed for v2.x.

**Scope — out**:
- CFS. We skip it entirely; EEVDF replaces it in Linux already.
- Plan 9's original scheduler. The decay-based priority adjustment is replaced by EEVDF's deadline computation; the rest of Plan 9's scheduler interface (`sched`, `ready`, `sleep`, `wakeup`) remains.

**Done definition**:
- Phase 2 exit: single-CPU scheduler runs N processes concurrently with bounded latency; timer preemption works; rfork/exits/wait lifecycle works; spec `scheduler.tla` clean.
- Phase 8 v1.0 exit: 8-CPU SMP scheduler runs 1000 processes in stress test for 72 hours without panic, deadlock, or unbounded latency (p99 latency stays within 2× of single-CPU baseline).
- Wake-up race test: 1000 processes blocking and being woken in tight loops; no missed wakeups (verified by counter).
- Work-stealing test: 4 CPUs, 1 of which has 100 processes ready, 3 idle — work redistributes within 5ms.
- TSan clean.

**Dependencies**:
- ARM generic timer driver (Phase 1).
- ARM64 context save/restore (Phase 2).
- IPI infrastructure (Phase 2 for SMP).
- Wait-queue + wakeup machinery (Phase 2).

**Complexity**:
- Per-CPU run tree (red-black tree wrapper): ~500 LOC.
- EEVDF eligibility / deadline computation: ~500 LOC.
- Three-band integration: ~300 LOC.
- Work-stealing logic: ~500 LOC.
- IPI infrastructure: ~500 LOC.
- Wakeup atomicity machinery: ~400 LOC.
- Tickless idle: ~300 LOC.
- Stats / observability: ~300 LOC.
- Plan 9 idiom layer (`sched`, `ready`, `sleep`, `wakeup`, `rendezvous`): ~500 LOC.
- Spec `scheduler.tla`: ~300-500 lines TLA+.
- Total: 4–6 KLOC C99 + ~500 LOC TLA+.

**Risk — Medium**:
- EEVDF is a published algorithm with Linux production deployment (since 6.6). Implementation risk is moderate.
- SMP correctness is the historical kernel-bug nexus. TSan + spec + adversarial audit are the mitigations.
- Wakeup atomicity is subtle. The spec catches it before code.
- 8-CPU exit criterion at Phase 8 is a real bar; missing it means scheduler debugging into Phase 8.

**Alternative approaches considered**:
- **Plan 9's original priority-decay scheduler**. Rejected: simple but doesn't have EEVDF's provable latency properties; for an OS targeting interactive workflows, the latency bound matters.
- **CFS** (Linux pre-6.6). Rejected: replaced upstream; no point implementing the predecessor.
- **MLFQ-3** (textbook). Considered (was my initial proposal in the challenge round). Rejected: less rigorous than EEVDF; EEVDF's deadline math is no more complex to implement.
- **BFS / MuQSS** (Con Kolivas's single-runqueue alternatives). Rejected: design controversies; less peer-review than EEVDF.
- **Custom Plan 9-shaped scheduler** (e.g. priority-queue-based). Rejected: reinventing prior art when EEVDF fits the use case.

**Sequence**: Phase 2 (core scheduler, single-CPU) → Phase 2 exit (SMP enabled, 4-CPU baseline) → Phase 8 (8-CPU stress + tuning).

---

### 3.7 Angle #7 — SOTA security hardening from day one

**Why it's novel**: most OS projects accumulate hardening as opt-in features added over years (Linux added KASLR in 4.x, CFI in 5.x; OpenBSD did W^X first; various BSDs added stack canaries piecemeal). Plan 9 / 9Front have minimal hardening. seL4 + Fuchsia have most modern hardening. **No Plan 9-heritage OS has the full modern hardening stack.** Thylacine builds with: KASLR, ASLR, W^X, compiler CFI, stack canaries, ARM PAC, ARM MTE, ARM BTI, LSE atomics — all enabled by default in v1.0 builds.

This is "table stakes" security in 2026 but it's a contribution because the prior Plan 9 lineage has none of it.

The model:

- **KASLR**: kernel image base randomized at boot. Symbol resolution via dynamic relocations.
- **Userspace ASLR**: musl ld.so randomizes load base of dynamic binaries; heap, stack, mmap regions randomized per-process.
- **W^X**: enforced as enumerated invariant (VISION I-12). Pages writable XOR executable, never both. JIT compilers (none in v1.0) need explicit syscall mediation.
- **Compiler CFI**: kernel + userspace built with Clang `-fsanitize=cfi`. Catches indirect-call hijacking.
- **Stack canaries**: kernel + userspace, default-on.
- **ARM PAC** (Pointer Authentication, ARMv8.3): return-address signing on kernel + userspace stacks.
- **ARM MTE** (Memory Tagging Extension, ARMv8.5): hardware memory tagging, default-on where supported (Apple Silicon, QEMU emulation). Catches UAF / overflow at hardware speed.
- **ARM BTI** (Branch Target Identification, ARMv8.5): branch target valid markers; indirect branches must land on a marked instruction.
- **LSE atomics** (Large System Extensions, ARMv8.1+): hardware atomic ops; runtime-detected via `HWCAP_ATOMICS`; LL/SC fallback.

**Scope — in**:
- Compiler flags applied to kernel build: `-fsanitize=cfi -fstack-protector-strong -mbranch-protection=standard -fPIE` etc.
- Kernel link: PIE + KASLR-compatible relocation.
- Page table entry bits: writable + executable mutually exclusive at the architecture layer.
- ARM `MAIR_EL1` configured for MTE (when available); page table bits flag the tag for each kernel-allocated page.
- ARM `SCTLR_EL1.BT = 1` (BTI for kernel).
- LSE detection at boot; kernel atomic primitives use LSE if available.
- musl + dynamic linker built with the equivalent userspace flags.
- ELF loader rejects RWX segments at load time.
- `mprotect` syscall rejects W^X-violating transitions (e.g. R+W → R+X).

**Scope — deferred** (post-v1.0):
- Hardware Branch Target Buffer (BTB) entries for indirect-call CFI on ARM (currently software-CFI only).
- Speculative execution mitigations beyond what hardware enables by default (hardware-mitigated on M1+).
- IPE (Integrity Policy Enforcement) policies for binary integrity at exec time.

**Scope — out**:
- Mandatory access control (SELinux / AppArmor equivalent). The namespace model provides isolation; MAC is a different threat model. v2.x consideration.
- Kernel module signature verification. v1.0 has no loadable kernel modules; the question is moot.

**Done definition**:
- All v1.0 binaries (kernel + userspace) ship with KASLR/ASLR/W^X/CFI/canaries/PAC/BTI on by default.
- MTE on by default where hardware supports; runtime detection at boot.
- LSE atomic primitives on Apple Silicon (verified via `objdump`).
- Test: a deliberate UAF in a test program is detected by MTE (program receives SIGSEGV with MTE tag mismatch info).
- Test: an attempt to `mprotect(addr, len, PROT_READ | PROT_WRITE | PROT_EXEC)` is rejected.
- Test: a forged kernel return address (PAC mismatch) panics the kernel cleanly with an attestation message.
- Boot KASLR: kernel base address differs across boots (verified via `/ctl/kernel/base`).
- Phase 7 audit pass on the security stack (the comprehensive hardening + audit + 8-CPU stress phase).

**Dependencies**:
- Compiler toolchain (Clang, supports all the flags).
- ARM64 exception-level setup (Phase 1).
- `mprotect` syscall (Phase 5).
- ELF loader (Phase 2).

**Complexity**:
- Compiler flag plumbing in build system: ~200 LOC of CMake.
- KASLR relocation: ~500 LOC C99.
- W^X enforcement at page-table layer: ~300 LOC C99.
- MTE setup + per-allocation tagging: ~500 LOC C99.
- BTI compile + runtime check: ~200 LOC C99.
- LSE detection + dispatch: ~300 LOC C99.
- PAC for kernel stacks: ~300 LOC asm + ~200 LOC C99.
- ELF loader RWX rejection: ~200 LOC C99.
- `mprotect` W^X enforcement: ~200 LOC C99.
- Total: 3–5 KLOC C99 + asm + build config.

**Risk — Low-medium**:
- Each individual hardening feature is well-understood with prior production deployment in Linux / Fuchsia / Chrome.
- Risk is integration complexity — multiple features interacting in unexpected ways. Mitigated by early-Phase-1 enablement so problems surface immediately.
- MTE has known performance overhead (~5-15% on tagged allocations) that must be measured. If significant, MTE on for kernel allocations only (default on) and userspace opt-in.
- KASLR boot-time symbol resolution adds a few milliseconds to boot; within the budget.

**Alternative approaches considered**:
- **Hardening as opt-in build flag**. Rejected: SOTA tenet — default build is the hardened build.
- **Software CFI only, no PAC/MTE/BTI**. Rejected: Apple Silicon has all of these; not using them would be deliberately leaving security on the table.
- **IPE / SBoMC / TPM-based attestation**. Deferred: these are deployment policies; v1.0 ships with the build-time hardening.

**Sequence**: Phase 1 (build flags + KASLR + basic ARM hardening) → Phase 2 (W^X + ELF loader + mprotect) → Phase 7 (final audit pass + measurement; v1.0-rc release tag).

---

### 3.8 Angle #8 — Formal verification cadence: nine TLA+ specs gate-tied to phases

**Why it's novel**: seL4 has full functional correctness in Isabelle (heroic, multi-decade effort). Linux has no formal verification. Fuchsia has scattered TLA+ for some protocols. Plan 9 / 9Front have none. **Practical TLA+ verification of every load-bearing OS invariant — gate-tied to phases, with the spec mandated before the implementation, with CI-gated TLC runs on every PR — is rare even in well-funded projects.** Stratum proved the model works for a filesystem; Thylacine applies it to an OS.

The nine specs:

| # | Spec | What it proves | Phase |
|---|---|---|---|
| 1 | `scheduler.tla` | Per-CPU EEVDF correctness, IPI ordering, wakeup atomicity, work-stealing fairness | Phase 2 |
| 2 | `namespace.tla` | bind/mount semantics, cycle-freedom, isolation between processes | Phase 2 |
| 3 | `handles.tla` | Rights monotonicity, transfer-via-9P invariant, hardware-handle non-transferability | Phase 2 |
| 4 | `vmo.tla` | Refcount + mapping lifecycle, no-use-after-free | Phase 3 |
| 5 | `9p_client.tla` | Tag uniqueness per session, fid lifecycle, out-of-order completion correctness, flow control | Phase 4 |
| 6 | `poll.tla` | Wait/wake state machine, missed-wakeup-freedom across N fds | Phase 5 |
| 7 | `futex.tla` | FUTEX_WAIT / FUTEX_WAKE atomicity (no wakeup lost between value check and sleep) | Phase 5 |
| 8 | `notes.tla` | Note delivery ordering, signal mask correctness, async safety | Phase 5 |
| 9 | `pty.tla` | Master/slave atomicity, termios state transitions | Phase 5 |

**Scope — in**:
- All nine specs written before their respective phase exits.
- Each spec has a `*.tla` (model), `*.cfg` (TLC config), and `*_buggy.cfg` (config that demonstrates a specific bug at the spec level — the executable-documentation pattern from Stratum).
- TLC clean under bounded parameters (4 threads, 8 procs, 16-step traces typical) in < 10 min on a developer laptop.
- CI runs TLC on every PR touching specified subsystems; failing TLC blocks merge.
- `SPEC-TO-CODE.md` maps each spec action to source locations; CI verifies the mapping is current.
- Adversarial audit cadence per CLAUDE.md: every change to invariant-bearing surfaces gets a focused soundness audit before merge.

**Scope — deferred** (post-v1.0):
- Coq / Lean / Isabelle proofs of full functional correctness. seL4-class effort; out of scope for Thylacine v1.0. v2.x candidate for narrow subsystems (handle table, 9P client) where the proof would catch bugs the spec doesn't.
- Refinement proofs (showing the C implementation refines the TLA+ spec). The spec-to-code mapping is human-maintained at v1.0; mechanical refinement is post-v1.0.

**Scope — out**:
- Full functional correctness of every kernel function. Out-of-scope; tests + sanitizers cover this.
- Verification of crypto primitives. Stratum's job (delegated to upstream libsodium / liboqs).

**Done definition**:
- Nine specs written, TLC-clean.
- Each spec has a buggy-config counterexample.
- CI integration: TLC runs on every PR touching specified files.
- Phase 8 v1.0 exit: every load-bearing invariant from VISION §8 maps to a spec or a documented runtime check.
- Audit log: at least one P0 or P1 bug found by a spec before implementation reached code (the "earned its keep" criterion from Stratum).

**Dependencies**:
- TLA+ tooling installed (TLC, TLA+ Toolbox).
- Spec-writing discipline maintained from Phase 0 onward.

**Complexity**:
- `scheduler.tla`: ~500 lines.
- `namespace.tla`: ~400 lines.
- `handles.tla`: ~400 lines.
- `vmo.tla`: ~350 lines.
- `9p_client.tla`: ~600 lines (most complex; multi-tag pipelining).
- `poll.tla`: ~400 lines.
- `futex.tla`: ~300 lines.
- `notes.tla`: ~400 lines.
- `pty.tla`: ~350 lines.
- Total: ~3.7 KLOC TLA+.
- CI integration scripts: ~300 LOC bash/Python.
- `SPEC-TO-CODE.md` per spec: ~100-200 lines of mapping per spec.
- Total: 4–6 KLOC TLA+ + scaffolding.

**Risk — Low-medium**:
- TLA+ is mature (Lamport, 25+ years). Production users: Amazon S3, DynamoDB, Azure Cosmos DB, MongoDB, CockroachDB, Stratum.
- Risk is the project team's ability to write correct, useful specs. Mitigated by starting with `scheduler.tla` (well-studied class of protocol) and iterating; using Stratum's existing specs as templates.
- Risk is keeping specs in sync with code as the implementation evolves. Mitigated by `SPEC-TO-CODE.md` maintenance + CI.

**Alternative approaches considered**:
- **Coq / Lean / Isabelle for full correctness**. Rejected for v1.0 (seL4-class effort). v2.x candidate.
- **Property-based tests + fuzzers as the only formal-ish verification**. Rejected: fuzzers find bugs but don't prove their absence; specs prove what fuzzers can't.
- **Alloy** (bounded model checking). Considered; less well-suited to liveness properties than TLA+. TLA+ is the better fit for OS protocols.
- **No formal verification, rely on tests + audits**. Rejected: SOTA tenet; the audit-only model has missed bugs Stratum's specs caught.

**Sequence**: Continuous from Phase 0. `scheduler.tla` and `namespace.tla` and `handles.tla` are Phase 2 deliverables. `vmo.tla` is Phase 3. `9p_client.tla` is Phase 4. `poll.tla`, `futex.tla`, `notes.tla`, `pty.tla` are Phase 5.

---

### 3.9 Angle #9 — Designed-not-implemented v2.0 contracts

**Why it's novel**: most OS projects either implement features or punt them entirely. The "contract is binding now; implementation lands later" pattern is rare. Stratum's roadmap uses it (e.g. zoned-storage support is contracted but post-v2.0). **For an OS, the pattern is uniquely valuable** because architectural commitments early on prevent v2.0 features from being painful retrofits.

The three contracts:

**A. Capability elevation via factotum** (`ARCHITECTURE.md §15.4` design):
- v1.0 has no setuid, no privilege elevation. Programs needing elevation run as elevated users from creation.
- v2.0 adds factotum-mediated elevation: a privileged 9P server (factotum/janus-cap) that, given a request from a process, grants a time-bounded capability handle that elevates specific operations.
- The contract: capability elevation is *always* mediated by a userspace 9P server, never directly by the kernel. setuid is *never* added.
- v1.0 work: design the protocol, write the spec sketch in `ARCHITECTURE.md`, ensure the kernel doesn't grow a setuid hook that would have to be removed later.

**B. Multikernel SMP** (`ARCHITECTURE.md §20.6` design):
- v1.0 is single-kernel-with-per-CPU-data-discipline. SMP scales to ~8 cores via per-CPU run queues + work-stealing.
- v2.x explores per-core kernel instances (Barrelfish-style) communicating via 9P. The natural extension of Thylacine's model: cross-core IPC becomes 9P, just as cross-process IPC is 9P.
- The contract: the per-CPU data discipline at v1.0 is structured so that v2.x multikernel is a configuration extension, not a rewrite. Per-CPU data is *not* shared by default; cross-CPU operations are explicit IPIs.
- v1.0 work: architectural sketch in `ARCHITECTURE.md §20.6`; per-CPU data discipline followed in implementation.

**C. In-kernel Stratum driver** (`ARCHITECTURE.md §14.4` design):
- v1.0 mounts Stratum via the kernel's 9P client. Each operation is one 9P round-trip.
- v2.0 (or v2.1) adds an in-kernel Stratum driver: bypass the 9P round-trip for root FS operations by linking part of `libstratum.a` into the kernel.
- The contract: the kernel's filesystem-mounting interface is generic enough that an in-kernel Stratum driver registers itself just like any other Dev would. No Stratum-specific syscall surface.
- v1.0 work: design sketch in `ARCHITECTURE.md §14.4`; ensure the mount infrastructure doesn't have 9P-specific assumptions baked in.

**D. Capability-based kernel addressing** (added at P3-Bb design discussion):
- v1.0 uses a Linux/FreeBSD-style kernel direct map: physical RAM linearly mapped at `0xFFFF_0000_*` in TTBR1; kernel allocators return pointers into the direct map; PA↔KVA conversion is constant-offset arithmetic. Pragmatic, well-understood, performant.
- v2.x explores a capability-based kernel addressing discipline inspired by seL4 (capability-mediated kernel memory) and CHERI (hardware-tagged pointers with bounds + permissions). The natural extension of Thylacine's existing handles.tla discipline applied INWARD: the same capability-monotonicity rules that govern user-facing handles would govern kernel-internal addressing.
- The contract: the v1.0 kernel direct map is **explicitly the pragmatic compromise**, not the principled SOTA. The kernel allocator's API surface (`kmem_cache_alloc`, `alloc_pages`, `kmalloc`) is structured so that v2.x can swap the implementation behind it from "raw void * pointers into direct map" to "typed kernel capabilities with explicit bounds + permissions" without callers needing rewrites — the call sites use the cache + size + flags interface, not raw arithmetic on PAs.
- The honest framing: the direct map exposes every byte of RAM to every line of kernel code (full speculative-load attack surface; no subsystem differentiation between filesystem cache vs process credentials). seL4's principled answer (no kernel direct map; explicit capability-mediated allocation) is multi-year and requires Rust (or CHERI hardware) — C's type system can't express provably-safe capabilities. v2.x is the right horizon.
- v1.0 work: this NOVEL section (the contract); a brief paragraph in `ARCHITECTURE.md §6` referencing it; ensure SLUB / buddy / VMO call sites use the capability-amenable API surface (already true at v1.0); document the SLUB PA-as-VA convention's removal at P3-Bb so the path is reversible.
- v1.0 commitment that survives: every direct-map PTE is **R/W + XN unconditionally** — kernel direct map is data, never code. W^X invariant I-12 holds at the alias level (the same physical page mapped R/X via kernel image VA is mapped R/W + XN via direct map; never both). KASLR-randomization of the direct-map base is deferred to a v1.x hardening pass (the offset is currently fixed at `0xFFFF_0000_0000_0000`).

**Scope — in** (Phase 0 design effort):
- `ARCHITECTURE.md §15.4` — capability elevation via factotum: protocol sketch, threat model, granted-capability shape, integration with `rfork` flags, interaction with namespaces.
- `ARCHITECTURE.md §20.6` — multikernel SMP direction: per-core kernel instance shape, cross-core 9P channels, hot-plug handling, NUMA story.
- `ARCHITECTURE.md §14.4` — in-kernel Stratum driver: shared-library linkage model, kernel-side handle types, integration with existing Dev infrastructure.
- `ARCHITECTURE.md §6.10` — capability-based kernel addressing direction (added at P3-Bb): the v2.x principled alternative to the direct map; references seL4 / CHERI; identifies what v1.0 commits that survives the migration vs what changes.

**Scope — deferred** (the implementations themselves):
- All three implementations are post-v1.0.

**Scope — out**:
- Implementing any of these at v1.0 would expand v1.0 scope by ~30-50 KLOC and ~2 years; out.

**Done definition**:
- All four sections in `ARCHITECTURE.md` are at COMMITTED status by their respective gates (the original three at Gate 3; §6.10 added during Phase 3 design when the direct-map vs capability question surfaced).
- Each section is detailed enough that the v2.x implementation team (which may include this same agent in a later session) can build to spec without redesign.
- Each section's design is consistent with v1.0 invariants — no v1.0 commitment that would need to be broken to implement the v2.x feature.

**Dependencies**:
- ARCH §15 (security model) for §15.4.
- ARCH §20 (per-core SMP) for §20.6.
- ARCH §14 (filesystem integration) for §14.4.
- ARCH §6 (memory management) for §6.10 — the kernel direct map design lives here; the capability-addressing direction is its v2.x successor.

**Complexity**:
- Phase 0 + Phase 3 design effort: ~500-1000 lines of `ARCHITECTURE.md` content per contract; total ~3-4 KLOC of architecture document.
- v1.0 implementation impact: ~0 KLOC (no code; architectural discipline only) — except for §6.10 where v1.0 implements the direct map (P3-Bb) with the v2.x-compatible API surface.

**Risk — Low**:
- The contracts are bounded scope statements, not promises of correctness.
- Risk is that the contracts turn out to be wrong (e.g. the multikernel sketch implies a kernel-level redesign that v1.0 didn't anticipate). Mitigation: revisit contracts at v1.0 release; if implementation discovery shows they need amendment, amend before v2.0 work begins.

**Alternative approaches considered**:
- **No contracts; figure out v2.0 features when v2.0 starts**. Rejected: v1.0 architectural decisions inform v2.0 features; without contracts, v1.0 risks decisions that paint v2.0 into a corner.
- **Implement all three at v1.0**. Rejected: ~30-50 KLOC of additional v1.0 scope; v1.0 release pushes out by ~2 years.

**Sequence**: Phase 0. The contracts are part of the Gate 3 scripture commitment.

---

## 4. Sequencing

Natural order, derived from the dependencies:

### Phase A — Foundations (Phases 1-2 of ROADMAP)

- **Angle #7 — SOTA security hardening.** Build flags + KASLR + ARM hardening in place from Phase 1. Continued through Phase 2 (W^X enforcement).
- **Angle #1 — 9P as universal composition.** Kernel `Dev` framework + `Chan` from Phase 1. Foundational for everything.
- **Angle #6 — EEVDF scheduler.** Phase 2 deliverable.
- **Angle #2 — Userspace driver infrastructure.** Phase 2 produces the handle table + VMO manager + IRQ forwarding; Phase 3 lights up userspace drivers.
- **Angle #8 — Formal verification.** `scheduler.tla`, `namespace.tla`, `handles.tla` at Phase 2.

### Phase B — Storage + 9P (Phase 4 of ROADMAP)

- **Angle #3 — Pipelined 9P client.** Phase 4 deliverable, foundational for Phase 4-8.
- **Angle #5 — Stratum integration.** Phase 4 deliverable.
- **Angle #8 — Formal verification.** `vmo.tla` at Phase 3, `9p_client.tla` at Phase 4.

### Phase C — Userspace + Utopia (Phases 5-7 of ROADMAP)

- **Angle #1 — POSIX surfaces as 9P servers.** Phase 5-7 deliverables. The POSIX surfaces (`/proc`, `/dev/pts/`, `/sys`, `/run`) all arrive as 9P servers in this phase block.
- **Angle #8 — Formal verification.** `poll.tla`, `futex.tla`, `notes.tla`, `pty.tla` at Phase 5.
- **Utopia milestone (Phase 5 exit)**: the textual POSIX environment is complete and "feels real, not broken." musl port + uutils-coreutils (full flag coverage, no subset) + `rc` + `bash` + `vim` + `tmux` + `ssh` + `git` + `make` + `python3` + `gcc`/`clang` all run. BusyBox in initramfs as the recovery shell. The user can use Thylacine for real work via SSH or UART console at this point — subsequent phases (Linux compat at Phase 6, hardening at Phase 7, Halcyon at Phase 8) are additive over Utopia, not replacements. Utopia is the v0.5-equivalent visible deliverable.

### Phase D — Linux compat + network (Phase 6 of ROADMAP)

- **Angle #1 — Linux-shaped POSIX surfaces** (`/proc-linux`, `/sys-linux`, `/dev-linux`) as 9P servers. Phase 6 deliverable.
- **Linux ARM64 binary shim.** Top-50 syscall coverage. Phase 6 deliverable.
- **OCI container as namespace.** Phase 6 deliverable.
- **Network stack** (smoltcp Rust port, with Plan 9 IP fallback option). Phase 6 deliverable.

### Phase E — Hardening + audit + v1.0-rc (Phase 7 of ROADMAP)

- **Angle #7 — Final security audit pass** across every audit-trigger surface from `ARCHITECTURE.md §25.4`.
- **Angle #8 — Spec-to-code mapping verified end-to-end** for all 9 TLA+ specs.
- **Angle #6 — 8-CPU SMP 72-hour stress.**
- All latency budgets (VISION §4.5) measured and gated.
- Fuzzer integration (1000+ CPU-hours per surface).
- **v1.0-rc release tag at Phase 7 exit** — a complete, hardened, audited textual + compat OS. If Halcyon (Phase 8) slips, this v1.0-rc ships as v1.0 and Halcyon becomes v1.1.

### Phase F — Halcyon + v1.0 final (Phase 8 of ROADMAP)

- **Angle #4 — Halcyon.** Phase 8 deliverable. The marquee feature; also the riskiest. Lands on top of the hardened, audited substrate.
- **Angle #1 — Halcyon as 9P client.** Same phase.
- **Halcyon-surface audit pass.** Halcyon-introduced surfaces (scroll-buffer state machine, image decode, video player 9P client, framebuffer driver extensions) get their own focused audit round.
- **v1.0 final release** at Phase 8 exit.

### Phase 0 (ongoing) — Designed-not-implemented contracts

- **Angle #9 — three contracts** in ARCHITECTURE.md by Gate 3.

---

## 5. If we're cut for time

If schedule pressure forces cuts, the priority for cutting:

1. **First to cut**: Hardware video decode (within angle #4 Halcyon). Software decode is sufficient at v1.0; HW decode is post-v1.0 anyway.
2. **Second**: 8-CPU SMP at Phase 8. Drop to 4-CPU stress as v1.0 release exit; 8+ becomes v1.1.
3. **Third**: Some POSIX features (within angle #1). The minimum viable set per ARCH §23.8 is non-negotiable; "should-have-before-Phase-6" items can slip.

Not cuttable:

- **Angle #1 (9P totalization)** — without this, the project's thesis fails.
- **Angle #2 (userspace drivers + handles)** — architectural; in-kernel-driver retrofit is a v1.0 failure.
- **Angle #3 (pipelined 9P)** — without this, latency budget fails.
- **Angle #5 (Stratum integration)** — Thylacine without Stratum is not Thylacine.
- **Angle #6 (EEVDF)** — without EEVDF, the latency story is on Plan 9-era scheduling.
- **Angle #7 (SOTA hardening)** — table stakes 2026.
- **Angle #8 (formal verification)** — methodology; cutting this drops Thylacine to Plan 9-era audit posture.
- **Angle #9 (v2.0 contracts)** — Phase 0 work; not cuttable without leaving v2.0 architecturally undefined.
- **Angle #4 (Halcyon) cannot be cut entirely** but the bash-subset can be reduced; the inline-graphics core ships.

If we hit scope pressure, we cut HW video decode → 8-CPU stress → POSIX edges in that order. Past that, we re-plan.

---

## 6. Novel-angle risk summary

| Angle | Risk | Fallback if it fails |
|---|---|---|
| #1 9P totalization | Low | Ad-hoc subsystem at first, refactor to 9P later (would re-enter the audit loop) |
| #2 Userspace drivers + handles | Medium | Selective kernel drivers at v1.0; userspace from v1.1 |
| #3 Pipelined 9P | Low-medium | Naive serialized 9P (10× slower; would miss latency budget) |
| #4 Halcyon | Medium-high | Plan 9 Rio + 8½ port (reverts the "shell as UI" thesis; v1.0 still ships) |
| #5 Stratum native FS | Low | Mount from FUSE shim at v1.0; native at v1.1 |
| #6 EEVDF on Plan 9 base | Medium | MLFQ-3 fallback (less rigorous; meets latency budget probably) |
| #7 SOTA hardening | Low-medium | Subset of features (only Linux-baseline, no MTE) |
| #8 Formal verification | Low-medium | Audit-only model (Stratum-style without specs) |
| #9 v2.0 contracts | Low | Drop the v2.0 contracts; v2.0 redesigns from scratch (worse) |

Most fallbacks are unattractive enough that "this fails" should be treated as a project-level risk, not an angle-level one. The angles compose; if multiple fail, the project's identity erodes.

---

## 7. Summary claim

Nine novel angles, total ~50-70 KLOC of C99 + ~16-24 KLOC of Rust + ~4-6 KLOC of TLA+. One medium-high-risk item (#4 Halcyon — the most exposed design choice) with a clear fallback. Dependencies are well-understood; sequencing is natural. Every angle has a testable "done" definition. Nothing is hand-waving.

Thylacine v1.0 is ambitious but the ambition is bounded and verifiable. We know what we're signing up for. The thylacine runs again.
