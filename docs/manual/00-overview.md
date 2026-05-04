# 00 — Overview: Using Thylacine

Bird's-eye view of what it's like to use Thylacine OS. Updated per chunk during implementation; currently scaffolded ahead of Phase 5 (the first user-visible release, **Utopia**).

---

## What Thylacine is, from a user's perspective

Thylacine is an operating system for shell-driven development workflows on ARM64 hardware. It is **not** a desktop OS — there is no graphical windowing system, no web browser, no Office-suite-class applications. The graphical layer (Halcyon, Phase 8) is a scroll buffer with inline graphics: text and pixel-addressable regions in one stream, time-ordered, naturally scrollable.

If your workflow looks like `tmux` + `vim` + `bash` + occasional inline image / video, Thylacine fits. If your workflow needs side-by-side panes in independent windows, web browsers, or Office documents, Thylacine doesn't.

Thylacine speaks 9P everywhere: every kernel resource, every userspace service, every administrative interface is a 9P file tree you can browse with `ls`, read with `cat`, write with `echo`. There is no separate admin API, no separate driver-loading interface, no separate IPC framework. One mechanism.

The filesystem is **Stratum** — PQ-encrypted, formally verified, COW. It runs as a userspace 9P server; the kernel mounts it like any other 9P server.

---

## What you can do, by phase

Thylacine ships in phases. Each phase adds capabilities.

### v0.1 - v0.4 (Phase 1-4): no user-visible behavior

The kernel boots, schedules processes, mounts Stratum. There is no shell yet. Development-only.

### v0.5 — Utopia (Phase 5)

**The first user-visible release.** A complete textual POSIX environment.

You can:

- SSH into a Thylacine VM (or attach via UART console).
- Use a shell — `rc` natively, `bash` for POSIX compat.
- Run a complete coreutils suite (uutils-coreutils — Rust rewrite of GNU; complete flag coverage, not stripped).
- Compile and run C programs via `gcc` or `clang` against the musl libc port.
- Use `vim`, `less`, `top`, `htop`, `tmux`, `ssh`, `git`, `make`, `python3`.
- Run pipelines with redirection, job control (`Ctrl-Z`/`bg`/`fg`), `Ctrl-C` interruption.
- Open files on Stratum (the root filesystem) and trust crash safety + integrity verification.
- Run a static Linux ARM64 binary via the syscall translation shim.
- Browse synthetic filesystems: `ls /proc/<pid>/`, `cat /dev/random`, `echo restart > /ctl/drivers/virtio-blk`.

You cannot yet:

- Run pre-built Linux ARM64 dynamic binaries (best-effort only at Phase 5; full at Phase 6).
- Run OCI containers (Phase 6).
- Use the network for anything beyond loopback (Phase 6).
- Display images or video in the shell (Phase 8).

### v0.6 (Phase 6) — Practical working OS

You can additionally:

- Run pre-built Linux ARM64 static binaries (`curl`, `wget`, `redis-cli`, `python3`).
- Run OCI containers via `thylacine-run` — Alpine Linux, distroless images.
- Use the network (TCP/UDP) — `wget https://example.com`, `ssh` to external hosts.
- Browse Linux-shaped `/proc-linux/`, `/sys-linux/` for compat with Linux admin tools.

### v1.0-rc.1 (Phase 7) — Hardened, audited, shippable

The Phase 6 system, hardened. 8-CPU stress tested. Fuzz tested. Audit clean. **This is a real shippable release** — if Halcyon (Phase 8) hits a wall, v1.0-rc.1 ships as v1.0.

### v1.0 (Phase 8) — Halcyon + final

You can additionally:

- Use Halcyon as the graphical shell.
- `display image.png` to render a PNG inline in the scroll buffer.
- `play video.mp4` to play H.264 video inline.
- Edit files in `vim` with full color rendering.
- Compose multi-pane workflows via `tmux` running inside Halcyon.

You still cannot:

- Run a web browser (no windowing system; out of scope at v1.0 and beyond).
- Use a multi-pane IDE with overlapping windows (out of scope).
- Use Office-suite-class applications (out of scope).

---

## What's not coming (deliberately out)

Per `VISION.md §9` non-goals:

- Graphical windowing system (compositor, window manager, display server). Ever.
- Web browser, multi-pane IDE, Office-suite-class applications. By design — Halcyon doesn't host them.
- Audio at v1.0. Deferred to post-v1.0.
- Bluetooth, USB peripherals beyond keyboard/mouse, hardware sensors at v1.0. Deferred.
- Distributed / clustered OS. 9P over network is supported; OS doesn't manage clusters.
- `setuid` binaries — Plan 9 didn't have them; Thylacine doesn't either. Capability elevation via factotum is post-v1.0.
- Windows binary compatibility. Ever.
- x86-64 at v1.0. ARM64 only. v2.x port mechanical above `arch/`.

---

## Hardware support

- **Primary at v1.0**: QEMU `virt` machine, ARM64, under Hypervisor.framework on Apple Silicon Mac. Near-native performance via hardware virtualization.
- **Post-v1.0 / v1.1**: Bare metal Raspberry Pi 5. Same kernel binary as QEMU; delta is EL2→EL1 drop, mailbox framebuffer, RP1 Ethernet.
- **Post-v1.0 / v2.0**: Bare metal Apple Silicon, x86-64, RISC-V (mechanical above `arch/`).

---

## What it's built on

- **C99 kernel** (small, audited, formally specified for invariant-bearing parts).
- **Rust userspace** (drivers, Halcyon, network stack, video player) — borrow checker eliminates UAF / overflow CVEs.
- **Stratum** — the native filesystem (PQ-encrypted, formally verified, COW). External project; integrated via 9P.
- **musl libc** — POSIX compat; C programs build against it.
- **uutils-coreutils** — Rust rewrite of GNU coreutils; complete flag coverage; default at v1.0.

---

## Where to go next

Once v0.5 (Utopia) ships:

- New users: `01-getting-started.md` — install, boot, first login.
- Developers: `05-syscalls.md` for the full syscall reference; `06-posix-programming.md` for POSIX gotchas.
- Sysadmins: `07-stratum-admin.md` for the filesystem; `11-troubleshooting.md` for recovery.
- Linux refugees: `08-linux-binary-compat.md` for what runs and what doesn't.

---

## See also

- `docs/VISION.md` — the project's mission and constraints.
- `docs/ROADMAP.md` — what ships when.
- `docs/REFERENCE.md` — the technical reference (developers of Thylacine, not users).

---

## Revision history

| Date | Change | Reason |
|---|---|---|
| 2026-05-04 | Scaffolded (Phase 0 complete). | Bird's-eye user-facing overview. Per-topic pages appear as their surfaces ship. |
