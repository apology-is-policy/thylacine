# Thylacine OS — Vision

**Status**: Phase 0 draft. This document is the root of all subsequent design decisions.
**Companion documents**: `ARCHITECTURE.md`, `ROADMAP.md`

---

## 1. What Thylacine is

Thylacine is a Plan 9-heritage operating system targeting ARM64, designed to be a real OS — not a
toy, not a research prototype. It is built around three convictions:

1. **Plan 9's ideas were correct.** The namespace model, the "everything is a file" philosophy
   taken seriously rather than superficially, and per-process namespaces as the primary isolation
   primitive — these are better ideas than what Unix evolved into. The industry walked away from
   them. Thylacine doesn't.

2. **The shell is sufficient as a UI.** A graphical windowing system is not a prerequisite for
   a capable, pleasant computing environment. A shell that renders images, plays video, and
   composes interfaces from synthetic filesystems can do everything a modern user needs, without
   the complexity tax of a compositor, a scene graph, or a display server.

3. **The filesystem is the OS.** Stratum — Thylacine's native filesystem — is not an
   afterthought. It is the storage substrate the OS is designed to run on: COW, formally
   verified, post-quantum encrypted, 9P-native. The OS and the filesystem share a design
   philosophy and a protocol.

The name Thylacine is intentional. The thylacine (Thylacinus cynocephalus) was a marsupial apex
predator declared extinct in 1936 — beautiful, functional, lost not because it failed but because
the world stopped making room for it. Plan 9 is the thylacine of operating systems: declared dead,
occasionally glimpsed alive in the infrastructure of the systems that replaced it (9P in WSL, in
container runtimes, in distributed systems). Thylacine OS asks: what if it had lived?

---

## 2. The naming family

Names in this project share an emotional register: words that carry longing for something that
cannot be again.

| Component | Name | Meaning |
|---|---|---|
| Operating system | **Thylacine** | The lost animal; the unlived future |
| Filesystem | **Stratum** | A record of a different time, preserved in layers |
| Shell | **Halcyon** | The calm before; the impossible return |
| Key agent | **janus** | Two-faced; the boundary between worlds (inherited from Stratum) |

---

## 3. Design philosophy

### 3.1 Everything is a 9P server

Any kernel subsystem that exposes state or accepts commands does so as a synthetic filesystem
served over 9P. Device drivers, process state, network interfaces, the video player, the key
agent, administrative controls — all are 9P trees that can be mounted, browsed, scripted, and
composed.

This is not aesthetic. It means:
- One permission model for all resources.
- One IPC mechanism.
- One namespace operation (`bind`, `mount`) for composition.
- Drivers are userspace processes with no special kernel privilege.
- The entire system is observable and controllable from a shell.

### 3.2 Namespaces are the isolation primitive

Each process has its own namespace — its own view of the filesystem tree. Containers, sandboxes,
and compatibility environments are not special kernel features: they are processes with carefully
constructed namespaces. This is what Plan 9 had from the start. Thylacine inherits it unchanged.

### 3.3 The shell is the graphical environment

Halcyon is not a terminal emulator running inside a window manager. It is the graphical
environment. The display surface is a scroll buffer where each entry is either a text region or a
pixel-addressable graphical region. Images render inline. Video plays inline. Halcyon controls
scroll, history, and focus.

There is no compositor. There is no scene graph. There is no window manager. There is Halcyon.

### 3.4 Correctness before compatibility

Compatibility with Linux/POSIX binaries is a useful property, not a design constraint. The kernel
API is Plan 9-heritage first. Compatibility is provided by translation layers, not by designing
the kernel around a foreign ABI.

### 3.5 Userspace drivers

Kernel drivers are the exception, not the rule. The preferred driver model is a userspace process
that owns device memory and IRQs (via appropriate kernel primitives) and exposes the device as a
9P server. The kernel provides the scaffolding; the driver logic lives outside it. Crashes are
isolated. Updates don't require reboots. Auditing is tractable.

---

## 4. Target

### 4.1 Architecture

**Primary**: ARM64 (AArch64). No x86-64 initially.

ARM64 is chosen because:
- Thylacine's primary development machine is Apple Silicon (M-series Mac).
- QEMU's `virt` machine on ARM64 runs under Hypervisor.framework with hardware virtualization —
  no instruction-level emulation, near-native performance during development.
- The ARM64 ISA is clean: no segmentation, no 40 years of compatibility barnacles, a coherent
  exception level model (EL0 user, EL1 kernel, EL2 hypervisor), and hardware security extensions
  (PAC, MTE) that matter for Thylacine's security posture.
- x86's days as the primary platform are numbered. ARM64 will dominate.

x86-64 support is a post-v1.0 consideration. The abstractions built for ARM64 will be cleaner
for having not been contaminated by x86 requirements.

### 4.2 Development target

**Primary dev target**: QEMU `virt` machine, ARM64, on an Apple Silicon Mac.

The `virt` machine gives:
- VirtIO block, network, GPU (virgl), input — clean, well-documented, designed for this purpose.
- GIC interrupt controller.
- PL011 UART.
- VirtIO GPU with virgl for hardware-accelerated rendering via the host GPU.

The gap between `virt` and Apple Silicon bare metal is real and deferred. Bare metal support
is a post-v1.0 goal (dependent on Asahi Linux's work for the AGX GPU and AIC interrupt
controller).

### 4.3 Scale targets

- Single-machine OS. No distributed computing, no cluster management.
- Development machine and light server workloads.
- Hardware profile: 8–64 GiB RAM, NVMe or VirtIO block, single GPU.

---

## 5. Properties, ranked

In order of priority when they conflict:

1. **Correctness.** The kernel must not corrupt data, violate namespace isolation, or permit
   privilege escalation. Correctness is non-negotiable.
2. **Security.** Namespace isolation as the primary security boundary. Stratum's cryptographic
   integrity as the storage boundary. Userspace drivers as the fault isolation boundary.
3. **Simplicity.** The design should be explainable. A small kernel is a more correct kernel.
   Complexity is added only where it pays for itself in one of the above properties.
4. **Usability.** Halcyon should be a genuinely pleasant environment to work in. The graphical
   shell is not a compromise; it should be better than a terminal emulator for everything it does.
5. **Compatibility.** Linux/POSIX binary compatibility is useful. It is not load-bearing. Degraded
   compatibility is acceptable; degraded correctness or security is not.
6. **Performance.** Fast enough not to be the bottleneck. Not optimized at the expense of
   correctness or simplicity.

---

## 6. Non-goals

- **Graphical windowing system.** No compositor, no window manager, no display server. Halcyon
  is the display surface. If you need a windowing system, Thylacine is not your OS.
- **x86-64 support at v1.0.** ARM64 only for now. x86-64 is a post-v1.0 port.
- **Binary-perfect Linux compatibility.** Broad compatibility for static/musl-linked binaries,
  graceful degradation for glibc binaries. Not a goal to run every Linux binary perfectly.
- **Distributed or clustered OS.** Single machine. 9P can be used over a network, but Thylacine
  itself does not manage a cluster.
- **Windows binary compatibility.** Not in scope.
- **Sound system.** Deferred. The audio stack is a v2.0 consideration at the earliest.
- **NUMA optimization.** Single-socket machines only for v1.0.

---

## 7. Relationship to Plan 9 and 9Front

Thylacine is not a Plan 9 fork. It does not derive from Plan 9 source code. It is a new OS
that takes Plan 9's ideas seriously and implements them in 2026, on current hardware, with current
tools and current understanding.

Specifically inherited from Plan 9:
- The namespace model (per-process, composable via bind/mount).
- 9P as the universal IPC and resource protocol.
- The `Dev` vtable pattern for kernel devices.
- The principle that authentication and key management are separate services (→ janus).
- The synthetic filesystem as the administration interface (→ `/ctl/`).

Specifically not inherited:
- Plan 9's graphics model (rio/8½). Halcyon replaces it entirely.
- Plan 9's C dialect. Thylacine uses standard C99 in the kernel.
- Plan 9's network stack. Thylacine uses a modern TCP/IP stack.
- Plan 9's on-disk filesystem (cwfs, fossil). Stratum replaces it entirely.

9Front is acknowledged as the most vital continuation of Plan 9 ideas. Where 9Front has made
good decisions (improved USB, better hardware support, various protocol refinements), those
decisions are studied and respected. Thylacine does not compete with 9Front — it asks a
different question.

---

## 8. Relationship to Stratum

Stratum is Thylacine's native filesystem. This relationship is intentional and architectural:

- Stratum serves its administration surface over 9P. In Thylacine, this is native: the kernel
  mounts the Stratum server and exposes it as part of the root namespace.
- Stratum's per-connection namespace model maps directly to Thylacine's per-process namespace
  model.
- Stratum's `janus` key agent is a userspace 9P server — exactly the driver model Thylacine
  uses for everything.
- Stratum's formal verification posture (TLA+ specs, adversarial audit loop) is the model
  for Thylacine's own correctness work.

Stratum was not designed for Thylacine. It will fit as if it was.

---

## 9. Halcyon — the graphical shell

Halcyon deserves its own section in the vision document because it is the most unusual design
decision.

The premise: a shell that is also the graphical environment. Not a terminal emulator inside
a window manager — the shell itself renders images, plays video, and displays rich output,
with the scroll buffer as the display surface.

### 9.1 The scroll buffer model

The output stream is a sequence of entries. Each entry is either:
- A **text region**: rendered in the current font with standard terminal semantics.
- A **graphical region**: a fixed-dimension pixel buffer, rendered inline, scrolling naturally
  with the text stream.

When an image is displayed, it occupies N lines of the scroll buffer as a rendered bitmap.
When output scrolls, the image scrolls with it. History is preserved. The model is simple
enough to implement correctly and rich enough to replace a windowing system for daily work.

### 9.2 Video via synthetic filesystem

Video playback is not a Halcyon built-in. It is a 9P server:

```
/dev/video/player/
    ctl          ← write: play, pause, seek <seconds>, stop
    position     ← read: current position in seconds
    duration     ← read: total duration
    frame        ← read: current decoded frame as raw pixels
    audio/ctl    ← write: volume, mute
```

Halcyon mounts the video server and polls `frame`, blitting the result into a graphical region
in the scroll buffer. Hardware video decoding is a driver that exposes this interface. The shell
does not need to know about codecs.

### 9.3 What Halcyon is not

- Not a Wayland compositor.
- Not an X11 server.
- Not Rio or 8½.
- Not a terminal emulator running inside anything else.

### 9.4 Implementation

Halcyon is written in Rust. It depends on:
- The Thylacine kernel's framebuffer device (a 9P server).
- A font rendering library (fontdue or equivalent — no dependency on FreeType).
- A shell parser compatible with bash syntax for the interactive layer.

---

## 10. Summary

Thylacine OS is the operating system Plan 9 would have become if the industry had not walked
away. It is built from first principles, in the right language, for the right architecture,
with the right filesystem underneath it, with the right shell as the face of it.

It is ambitious. The ambition is bounded and sequenced.

The thylacine was real. So is this.
