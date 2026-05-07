# Thylacine OS — Development Tooling and Agentic Loop

**Status**: Phase 0 supplementary, 2026-05-04. This document describes the development environment, the QEMU-based feedback loop, and the agentic autonomy model that enables scalable kernel development.

**Companion documents**: `VISION.md`, `COMPARISON.md`, `NOVEL.md`, `ARCHITECTURE.md`, `ROADMAP.md`, `CLAUDE.md`.

**Audience**: Claude Code instances working on Thylacine, plus human developers who want to understand the development workflow.

Read this alongside `ROADMAP.md`. The tooling described here is not optional infrastructure — it is the mechanism that makes the phase plan executable.

---

## 1. The development target

Primary: **QEMU `virt` machine, ARM64**, running under Hypervisor.framework on an Apple Silicon Mac. ARM64 guest on ARM64 host = hardware virtualization, no instruction translation, near-native performance.

This means:
- Fast boot cycles (target: < 5 seconds from `qemu` invocation to shell prompt; tightens to < 3s at v1.0 per `VISION.md §4.5`).
- No SD cards, no physical hardware, no reflashing during development.
- Full control over the machine: pause, snapshot, restore, kill.
- The host filesystem is accessible to the guest via VirtIO-9P (see §4).

Do not overfit code to QEMU. All peripheral addresses, IRQ numbers, and memory regions are read from the DTB. No `#ifdef QEMU`. The same kernel binary targets QEMU `virt` and Raspberry Pi 5. See `ARCHITECTURE.md §22`.

---

## 2. The `tools/` directory

The `tools/` directory is a first-class part of the repository. It is not a collection of scripts that grew organically — it is designed infrastructure with the same discipline as the kernel source. Every tool is documented; every tool has a defined interface.

```
tools/
  run-vm.sh           ← launch QEMU with the correct flags
  deploy.sh           ← update kernel/binary in running VM via 9P share
  agent-protocol.md   ← the command/result convention for agentic interaction
  build.sh            ← full build wrapper around CMake + Cargo
  test.sh             ← run the test suite against a running VM
  snapshot.sh         ← save/restore QEMU VM state
  netboot/            ← Pi 5 TFTP netboot setup (post-v1.0 / v1.1 candidate)
```

These tools are written *before* Phase 1 kernel code. A working `run-vm.sh` that boots a minimal ARM64 stub and prints to UART is the first deliverable of the project. Everything else builds on it.

The build wrappers (`build.sh`, top-level `Makefile` targets) are thin shell wrappers around CMake (kernel, per `ARCHITECTURE.md §3`) and Cargo (Rust userspace). The wrappers exist to keep developer ergonomics simple; the underlying build systems are CMake + Cargo as committed in scripture.

---

## 3. `tools/run-vm.sh` — the canonical QEMU invocation

The VM is always launched via `run-vm.sh`. Direct `qemu-system-aarch64` invocations are not used — they diverge and accumulate inconsistencies.

**Canonical flags:**

```sh
qemu-system-aarch64 \
  -machine virt,gic-version=3 \         # GICv3 explicit (autodetect-friendly DTB)
  -cpu max \                            # all available ARM extensions for QEMU
  -smp 4 \                              # 4 vCPUs (default; up to 8 for SMP test)
  -m 2G \                               # 2 GiB RAM (default)
  -kernel build/kernel/thylacine.elf \  # ELF loaded directly, no bootloader
  -append "earlycon=pl011,0x09000000" \ # kernel cmdline
  -serial mon:stdio \                   # UART → host stdout; QEMU monitor on Ctrl-A
  -nographic \                          # no graphical window (Phase 1-5)
  -virtfs local,path=./share,\
    mount_tag=host0,security_model=none,id=host0 \  # 9P host share (see §4)
  -drive file=build/disk.img,\
    format=raw,if=virtio \              # VirtIO block device
  -netdev user,id=net0 \                # user-mode networking
  -device virtio-net-pci,netdev=net0    # VirtIO network
```

**`-cpu max`** is preferred over `-cpu cortex-a76` for development: it exposes every ARMv8.5+ extension QEMU emulates (PAC, MTE, BTI, LSE, SVE), which the hardening stack relies on per `ARCHITECTURE.md §24`. For Pi 5 hardware testing, the actual Pi 5 CPU (Cortex-A76, ARMv8.2) provides PAC+LSE but not MTE/BTI; the kernel detects and adapts at boot.

**Flags to add as phases progress:**

```sh
  -device virtio-gpu-pci \              # Phase 8: framebuffer (Halcyon)
  -display sdl \                        # Phase 8: display window on host
  -device virtio-keyboard-pci \         # Phase 8: keyboard input
  -device virtio-mouse-pci              # Phase 8: mouse input
```

`run-vm.sh` accepts arguments:
- `--snapshot <name>`: restore from a saved snapshot before boot.
- `--gdb`: expose GDB stub on port 1234 (`-s -S` flags), wait for debugger.
- `--cpus <n>`: override vCPU count. Default 4; use `--cpus 8` for SMP stress (Phase 8 exit criterion).
- `--mem <M>`: override RAM. Default 2G.
- `--no-share`: disable VirtIO-9P host share (for isolation testing).
- `--virgl`: enable VirtIO-GPU virgl 3D (post-v1.0).

---

## 4. The 9P host share — the hot-reload mechanism

**This is the key mechanism for fast agentic development.**

QEMU's `virtfs` exposes a host directory (`./share/`) inside the VM as a 9P filesystem, mounted at `/host` inside Thylacine. The agent writes new binaries to `./share/` on the host; the VM reads them from `/host/` without rebooting.

```
Host (macOS):          ./share/
                            drivers/virtio-blk    ← agent drops new binary here
                            kernel/thylacine.elf  ← new kernel (requires reboot)
                            tests/                ← test scripts and fixtures

VM (Thylacine):        /host/
                            drivers/virtio-blk    ← driver reads and re-execs
                            tests/                ← test runner reads
```

**Hot-reload flow for a userspace driver update:**

```sh
# On host (agent does this):
cargo build --release -p virtio-blk
cp target/aarch64-unknown-thylacine/release/virtio-blk share/drivers/

# In VM (agent sends this over serial/virtio-console):
echo "restart" > /ctl/drivers/virtio-blk
# or: kill old driver process, exec new one from /host/drivers/virtio-blk
```

No reboot required for userspace driver updates. The 9P host share makes the iteration loop — build, deploy, test — fast enough for agentic autonomy.

This integrates with the driver supervisor (`init/driver-supervisor.c`, Phase 3 deliverable per `ROADMAP.md §6.1`): the supervisor watches `/ctl/drivers/<name>` for `restart` writes and orchestrates the kill/exec dance, including handling 9P session re-establishment.

**Kernel updates require reboot:**

```sh
# On host:
make kernel
cp build/kernel/thylacine.elf share/kernel/

# Send reboot command to VM:
echo "reboot" > /ctl/system   # or QEMU monitor: system_reset
# QEMU reloads kernel from -kernel flag; but share is live immediately
```

For kernel development, use QEMU snapshots (§6) to checkpoint known-good states and roll back quickly after a panic.

---

## 5. The agent protocol — structured result convention

When a Claude Code agent sends commands to the VM and reads results, raw terminal output is fragile to parse. The following lightweight convention makes results machine-readable without requiring anything sophisticated from the VM side.

**All test/verification commands inside the VM are wrapped:**

```sh
# In VM, all commands run via:
thylacine_run() {
    local label="$1"; shift
    local output
    output=$("$@" 2>&1)
    local code=$?
    printf '\n===RESULT label=%s code=%d===\n' "$label" "$code"
    printf '%s\n' "$output"
    printf '===END %s===\n' "$label"
}

# Example:
thylacine_run "virtio-blk-read-test" \
    dd if=/dev/vda of=/dev/null bs=4096 count=256
```

**The agent parses for the structured markers:**

```
===RESULT label=virtio-blk-read-test code=0===
256+0 records in
256+0 records out
===END virtio-blk-read-test===
```

Exit code 0 = pass. Non-zero = fail. The label identifies the test. Output between the markers is the test's stdout/stderr.

This convention requires no kernel support — it is pure shell. It is available from the moment **Utopia** boots (Phase 5 exit per `VISION.md §13`). Before Utopia, the agent parses raw UART output for known boot messages and panic strings.

**Pre-Utopia (Phases 1-4): UART pattern matching**

Before the shell exists, the agent looks for:
- Boot success: `"Thylacine boot OK"` printed by `boot_main()` at the end of init (the boot banner contract — see §10).
- Kernel panic: `"PANIC:"` prefix, followed by the panic message.
- Phase exit criteria: specific strings printed by test code (e.g. `"TIMER: tick 1000"` for the Phase 1 timer test).

`boot_main()` must print a canonical success banner as its last act. This is not optional — it is the agent's signal that the boot succeeded.

---

## 6. QEMU snapshots — the kernel development safety net

Kernel development means frequent panics. Snapshots let the agent checkpoint known-good states and roll back without a full rebuild.

```sh
# Save snapshot (from QEMU monitor, or via QMP):
tools/snapshot.sh save phase1-boot-ok

# Restore and continue:
tools/snapshot.sh restore phase1-boot-ok
```

**Snapshot discipline:**
- Save a snapshot at every phase exit criterion that passes.
- Name snapshots descriptively: `phase1-uart-ok`, `phase2-first-process`, `phase3-virtio-blk-rw`, `phase4-stratum-mounted`, `phase5-utopia-ok`, etc.
- Never overwrite a passing snapshot with a broken state.
- The agent restores the last passing snapshot automatically after N consecutive panics (configurable, default: 3).

Snapshots are stored in `build/snapshots/` and are excluded from git (they are large and machine-specific).

---

## 7. The agentic development model

### 7.1 The crossover point: Utopia

The agentic autonomy model has two distinct phases separated by a hard threshold:

**Before Utopia (Phases 1-4): human-primary, agent-assisted**

The kernel skeleton, process model, and device layer require close human oversight. The feedback loop is slow (boot, observe, reboot), failure modes are catastrophic (kernel panic), and the knowledge required is specialized. The agent implements and tests, but the human reviews every significant architectural decision.

During this phase, the agent's primary job is:
- Implement to spec (`ARCHITECTURE.md` is the spec).
- Run phase exit criteria (`ROADMAP.md`) after each significant change.
- Report results clearly: pass/fail, panic message if applicable, UART output.
- Flag any implementation that requires departing from the spec.
- Never proceed past a panic without human review.

**After Utopia (Phases 5+): agent-primary, human-directed**

Once Utopia boots — a shell prompt exists, commands run, the structured result protocol works — the agent can operate with much greater autonomy:
- Implement a subsystem, deploy via 9P share, run tests, iterate.
- Run audit rounds on trigger surfaces (per `ARCHITECTURE.md §25.4`).
- Harden a driver by fuzzing it from the host side via the 9P session.
- Port a userspace tool (coreutils, bash) and verify it runs correctly.

The human reviews diffs and sets direction. The agent implements, verifies, and iterates. This is the same model that made Stratum's audit loop scalable.

### 7.2 The agentic loop — concrete workflow

```
┌─────────────────────────────────────────────────────────┐
│ 1. Human sets objective (e.g. "implement poll()")       │
└────────────────────┬────────────────────────────────────┘
                     ↓
┌─────────────────────────────────────────────────────────┐
│ 2. Agent reads ARCHITECTURE.md §23.3 (poll spec)        │
│    Checks ROADMAP.md Phase 5 exit criteria              │
│    Reviews existing kernel/spoor.c for readiness hooks   │
│    Confirms specs/poll.tla scaffolded                   │
└────────────────────┬────────────────────────────────────┘
                     ↓
┌─────────────────────────────────────────────────────────┐
│ 3. Agent writes/refines specs/poll.tla                  │
│    TLC clean before code starts                         │
│    Implements kernel/poll.c                             │
│    Writes test in tests/test_poll.c                     │
│    Builds: tools/build.sh kernel                        │
└────────────────────┬────────────────────────────────────┘
                     ↓
┌─────────────────────────────────────────────────────────┐
│ 4. Agent runs: tools/run-vm.sh                          │
│    Observes boot banner (or panic — restore snapshot)   │
│    Sends test command over serial                       │
│    Parses ===RESULT=== markers                          │
└────────────────────┬────────────────────────────────────┘
                     ↓
┌─────────────────────────────────────────────────────────┐
│ 5. Pass → save snapshot, report to human, next task     │
│    Fail → read panic/output, iterate from step 3        │
│    Repeated panic → restore previous snapshot, flag     │
└─────────────────────────────────────────────────────────┘
```

Step 3's spec-first discipline is non-negotiable for invariant-bearing changes per `ROADMAP.md §3.2`. The TLA+ model checker catches design errors at the spec level where they cost minutes, not at runtime where they cost commits.

### 7.3 Audit trigger surfaces

Certain changes require a dedicated soundness audit round before merge — the same principle as Stratum's audit loop. **The authoritative list is in `ARCHITECTURE.md §25.4`** (15 surfaces enumerated). The agent consults that list rather than the partial enumeration here.

The audit cadence:
1. Spawn a soundness-prosecutor agent (general-purpose subagent, `run_in_background: true`, maximum-capability model).
2. Include `memory/audit_rN_closed_list.md` contents as the "already fixed — do not re-report" preamble.
3. Scope the prompt to the changed surface.
4. Tell the agent explicitly to prosecute, not defend.
5. Wait for the completion notification.
6. Trust but verify: validate quoted file:line references.
7. Fix every P0/P1/P2 finding before merge. P3 findings get tracked or closed with explicit justification.

`memory/audit_rN_closed_list.md` is updated cumulatively — each round appends.

### 7.4 TLA+ spec requirement

The full list of nine TLA+ specs gate-tied to phases is in `NOVEL.md` Angle #8 and `ARCHITECTURE.md §25.2`:

| # | Spec | Phase |
|---|---|---|
| 1 | `specs/scheduler.tla` | 2 |
| 2 | `specs/territory.tla` | 2 |
| 3 | `specs/handles.tla` | 2 |
| 4 | `specs/burrow.tla` | 3 |
| 5 | `specs/9p_client.tla` | 4 |
| 6 | `specs/poll.tla` | 5 |
| 7 | `specs/futex.tla` | 5 |
| 8 | `specs/notes.tla` | 5 |
| 9 | `specs/pty.tla` | 5 |

The spec lives in `specs/<subsystem>.tla` (and a `.cfg` for TLC + a `_buggy.cfg` demonstrating the bug counterexample). The spec is the source of truth; the implementation is an implementation of the spec. If the implementation diverges from the spec, the spec wins.

CI runs TLC on every PR touching specified files; failing TLC blocks merge.

`SPEC-TO-CODE.md` per spec maps each TLA+ action to a source location. CI verifies the mapping is current — file must exist, function must exist, line range must match.

---

## 8. Raspberry Pi 5 — second hardware target

**When**: post-v1.0 (v1.1 candidate per `ROADMAP.md §12.1`). Pi 5 work *can* begin in parallel with later v1.0 phases if there's spare capacity, but it is not a v1.0 release deliverable. The reasoning: Phase 6 (Linux compat + network), Phase 7 (hardening + audit + 8-CPU stress + v1.0-rc), and Phase 8 (Halcyon + v1.0 final) are full-effort phases against QEMU `virt`; pulling in Pi 5 hardware bring-up at the same time risks all of them.

**Why Pi 5**:
- GIC-400 interrupt controller: identical to QEMU `virt` — GIC driver transfers directly with no changes.
- PL011 UART: identical to QEMU `virt` — UART driver transfers directly.
- ARM generic timer: identical — timer driver transfers directly.
- VideoCore VII GPU: open Mesa driver (v3d), Raspberry Pi published the spec.
- RP1 south bridge: datasheet published, Linux driver open source.
- Network boot via TFTP: fast iteration loop, no SD card swapping.
- M.2 NVMe HAT available: Stratum on real NVMe, full storage stack validation.
- Price: ~$80-120 for the board + HAT. Cheap enough to dedicate to CI.

**The delta from QEMU `virt` to Pi 5:**

| Component | QEMU `virt` | Pi 5 | Delta |
|---|---|---|---|
| Boot | QEMU `-kernel` direct ELF | U-Boot → kernel8.img | EL2→EL1 drop in `start.S` |
| UART | PL011 @ DTB address | PL011 @ DTB address | None — same driver |
| Interrupts | GIC-400/v3 @ DTB address | GIC-400 @ DTB address | None — same driver |
| Timer | ARM generic timer | ARM generic timer | None — same driver |
| Block storage | VirtIO-blk | NVMe via PCIe | New driver (userspace 9P server) |
| Network | VirtIO-net | RP1 Ethernet | New driver (for TFTP boot) |
| Framebuffer | VirtIO-GPU | VC7 mailbox | New driver (mailbox interface) |
| Display | QEMU SDL window | HDMI via RP1 | Behind mailbox framebuffer |
| Hardening features | `-cpu max` (PAC+MTE+BTI+LSE) | A76 (PAC+LSE; no MTE/BTI) | Runtime fallback per `ARCHITECTURE.md §24` |

The `arch/arm64/rpi5/` layer contains:
- `boot.S`: EL2→EL1 drop sequence. ~30 lines of assembly.
- `mailbox.c`: VideoCore mailbox interface for framebuffer allocation.
- `early_uart.c`: UART init before DTB is parsed (Pi 5 UART base address differs from QEMU; read from DTB after early init).

Everything in `arch/arm64/common/` is shared and untouched.

**Pi 5 network boot setup (one-time):**

```sh
# On macOS (host):
# 1. Configure Pi 5 EEPROM for network boot (one-time, via rpi-eeprom-config)
# 2. Run TFTP server on macOS pointing at tools/netboot/
# 3. tools/netboot/ contains kernel8.img (converted from ELF) + DTB
# 4. On each build: make piboot  → converts ELF, copies to netboot/, Pi picks it up

# Pi 5 boot sequence:
# Power on → EEPROM → DHCP → TFTP fetch kernel8.img + thylacine.dtb → boot
```

After one-time EEPROM setup, the Pi 5 development loop is:
```
make piboot → power-cycle Pi → observe UART output (USB serial cable to Mac)
```

No SD card swapping. Build-to-boot time: ~10 seconds.

**Pi 5 framebuffer driver:**

The QEMU VirtIO-GPU driver and the Pi 5 framebuffer driver expose the same 9P interface at `/dev/fb/`. Halcyon does not change between targets.

Pi 5 framebuffer via VideoCore mailbox:
```
init:
  send mailbox FRAMEBUFFER_ALLOCATE: width=1920, height=1080, depth=32
  receive: physical address, pitch
  map physical address into driver process VA (KObj_Burrow physical)
  expose /dev/fb/ 9P server as normal

/dev/fb/image write:
  memcpy into mapped framebuffer

/dev/fb/ctl "flush":
  ARM64 DC CIVAC cache flush on buffer range
  memory barrier (DSB ST)
  → display updates at next vsync
```

The mailbox interface abstracts the RP1 display controller. The driver does not touch RP1 registers directly for the framebuffer case.

**Pi 5 as a CI machine:**

Once network boot is configured, the Pi 5 can serve as a dedicated hardware CI target:
- Every commit that passes QEMU tests is automatically deployed to the Pi via `tools/netboot/`.
- Pi boots, runs the test suite over its UART serial connection, reports pass/fail.
- Hardware-specific failures (cache coherency, real interrupt timing, DMA alignment) surface here that QEMU's emulation would miss.

---

## 9. Build system

The top-level entry points (thin wrappers around CMake + Cargo per `ARCHITECTURE.md §3`):

```
tools/build.sh kernel       ← cmake + make for kernel ELF (C99, ARM64 cross-compile)
tools/build.sh sysroot      ← build musl + CRT + compiler-rt + libc++ sysroot
tools/build.sh userspace    ← cargo build --release --workspace for Rust components
tools/build.sh disk         ← assemble build/disk.img (Stratum volume + initramfs)
tools/build.sh all          ← kernel + sysroot + userspace + disk
tools/build.sh piboot       ← convert kernel ELF to kernel8.img for Pi 5 netboot (post-v1.0)
tools/test.sh               ← run-vm.sh + test runner (boots VM, runs test suite, reports)
tools/build.sh clean        ← remove build artifacts
```

A top-level `Makefile` provides `make kernel`, `make all`, `make test`, etc. as conventional aliases for the above.

Cross-compilation toolchain (installed on host macOS):
- `clang` with `--target=aarch64-unknown-thylacine` + `--sysroot=build/sysroot`
- `lld` as the linker
- Cargo with `aarch64-unknown-thylacine` target (custom target JSON)

The sysroot is built once and cached. `tools/build.sh sysroot` only reruns if musl or kernel headers change.

The kernel build uses Clang (not GCC) per `ARCHITECTURE.md §3` for CFI + ARMv8.5 feature support.

---

## 10. The boot banner contract

**This is non-negotiable for the agentic loop to work.**

`boot_main()` in `kernel/main.c` must print the following as its final act before entering the init process:

```
Thylacine vX.Y-dev booting...
  arch: arm64
  cpus: N
  mem:  XXXX MiB
  dtb:  0xADDR
  hardening: KASLR+ASLR+W^X+CFI+PAC+MTE+BTI+LSE+canaries
  kernel base: 0xADDR (KASLR offset 0xADDR)
Thylacine boot OK
```

The line `Thylacine boot OK` is the agent's boot-success signal. It must:
- Appear on a line by itself.
- Be the last kernel-printed line before userspace takes over.
- Not appear if the kernel panicked before completing init.

A kernel **extinction** (ELE — Extinction Level Event; the thylacine's own fate transposed onto a kernel that has lost the will to continue) must print:

```
EXTINCTION: <message>
  at <file>:<line>          (when registered with extinction_with_addr or richer variants land)
  <register dump>            (P1-F+, when exception infrastructure is in place)
  <PAC mismatch info if applicable>
  <MTE tag mismatch info if applicable>
```

The `EXTINCTION:` prefix is the agent's catastrophic-failure-detection signal. Any output matching `/^EXTINCTION:/` on the UART stream triggers the agent to: record the message, restore the last good snapshot, and report to the human before retrying.

The function is `extinction(const char *msg)` / `extinction_with_addr(const char *msg, uintptr_t addr)` in `kernel/extinction.c` (header `kernel/include/thylacine/extinction.h`). Convenience macro `ASSERT_OR_DIE(expr, msg)` for assert-style checks.

These two strings (the success banner and the EXTINCTION prefix) are part of the kernel ABI with the development tooling. They do not change without updating `tools/run-vm.sh`, `tools/test.sh`, `tools/agent-protocol.md`, and `CLAUDE.md` in the same commit.

---

## 11. Cross-references and where this fits

| Concern | Authority |
|---|---|
| Architectural decisions | `ARCHITECTURE.md` |
| Phase ordering and exit criteria | `ROADMAP.md` |
| Audit-trigger surface enumeration | `ARCHITECTURE.md §25.4` |
| TLA+ spec list and phase mapping | `ARCHITECTURE.md §25.2` + `NOVEL.md §3.8` |
| Hardening invariants | `ARCHITECTURE.md §24` + `VISION.md §8 (I-12, I-16)` |
| Driver model | `ARCHITECTURE.md §13` + `§9.3` + `§18` |
| Stratum coordination | `VISION.md §11` + `ROADMAP.md §7` |
| Utopia milestone definition | `VISION.md §13` + `ARCHITECTURE.md §23` |
| Memory protocol (CLAUDE.md) | `CLAUDE.md` (Phase 4 deliverable) |
| Boot banner ABI | This document, §10 |
| Agent protocol convention | This document, §5 |
| QEMU snapshot discipline | This document, §6 |

This document is supplementary — it doesn't override anything in the other scripture. Where this document and another scripture document disagree, the architectural document wins; this one updates to match.

---

## 12. Revision history

| Date | Change | Reason |
|---|---|---|
| 2026-05-04 | Initial draft (Phase 0 supplementary). | Drafted by Claude Desktop (Sonnet) as the missing tooling spec; threaded into Thylacine scripture with cross-references aligned to `ARCHITECTURE.md` (audit surfaces table, TLA+ spec list, build system commitment, hardening defaults). |
