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
  screendump.sh       ← QMP scanout capture to PNG (the "agentic eyes"; §3.1)
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

**Accel default (since Lazarus W3.5, 2026-06-06):** `run-vm.sh` **auto-detects HVF** on a capable host (Apple Silicon + `kern.hv_support` + a qemu built with hvf) and defaults to `accel=hvf` + `gic-version=2` + `-cpu host` — the fast dev/test loop, the M1 end-state (`PORTABILITY.md §8`). The `-machine virt,gic-version=3 -cpu max` (TCG) flags shown above are now the **occasional full-emulation compat reference** (the unique exerciser of RNDR / GICv3 / full-ISA, and the only host-portable path); force it with `THYLACINE_ACCEL=tcg` (or `make test-tcg` / `make run-tcg`). A non-Apple host auto-falls-back to TCG. `THYLACINE_ACCEL` / `THYLACINE_CPU` / `THYLACINE_GIC` override explicitly.

**`-cpu max`** is preferred over `-cpu cortex-a76` for the TCG compat reference: it exposes every ARMv8.5+ extension QEMU emulates (PAC, MTE, BTI, LSE, SVE), which the hardening stack relies on per `ARCHITECTURE.md §24`. Under HVF the guest sees the host CPU via `-cpu host` (Apple cores: LSE+PAC+BTI, no RNDR — the kernel CSPRNG seeds from virtio-rng instead, Lazarus W3). For Pi 5 hardware testing, the actual Pi 5 CPU (Cortex-A76, ARMv8.2) provides PAC+LSE but not MTE/BTI; the kernel detects and adapts at boot.

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

**Display backend (`THYLACINE_DISPLAY`, the fbcon era — #31):** headless
(`-nographic`) stays the default (the CI/agent loop). `=cocoa` opens the
interactive window — the human path to the Aurora console (switch the
window's View menu to the virtio-gpu console; serial stays on the
launching terminal). `=vnc:N` serves the gpu0 console on
`127.0.0.1:590N` — the headless live-display mode
(`tools/interactive/ls-gfx-live.exp` + `tools/rfb-refresh.py` drive it;
in this mode the vestigial `gpu-mmio0` device is dropped so the gpu0
console binds QemuConsole 0, which a VNC client attaches to).

### 3.1 Agentic eyes — `tools/screendump.sh` (QMP scanout capture)

The graphics arc's capture step (G-0; `TAPESTRY.md` §18.9). Captures the
running VM's virtio-gpu scanout to a PNG over the QMP socket `run-vm.sh`
opens by default (`build/qmp.sock`; disabled by `THYLACINE_NO_QMP=1`):

```bash
tools/screendump.sh out.png                 # capture gpu0 head 0
tools/screendump.sh -v out.png              # + verify the P4-L 4-quadrant
                                            #   test pattern (red/green/blue/white)
tools/screendump.sh -c out.png              # + verify the Aurora console (G-4:
                                            #   exact Bonfire bg dominant + fg text;
                                            #   G-5: + blend-integrity edges)
tools/screendump.sh -c -F frame.ppm         # offline: verify an existing P6 PPM
                                            #   (no QMP/VM; the regression path)
tools/screendump.sh -s SOCK -d DEV -H N ... # explicit socket / qdev id / head
```

Properties (all empirically verified at G-0):

- **Headless-safe.** QEMU maintains the QemuConsole surface for a bound
  scanout regardless of display backend, so capture works under the
  standing `-nographic` invocation — no `-display` change, no VNC needed.
  The capture targets the `gpu0` qdev id — since G-1 that is the
  `virtio-gpu-pci` device (owned by tapestryd since G-3; the one-shot
  kernel-test probe's virtio-mmio device is `gpu-mmio0`) — so console
  muxing never misroutes it. `tools/test.sh` runs the per-boot scanout
  gate on every boot (and therefore in every `ci-smp-gate` boot);
  `THYLACINE_GPU_GATE=0` opts out. Since G-4 the gate is `-c` (the
  Aurora CONSOLE signature — the boot presenter is the fbcon, not the
  demo pattern) plus a bounded retry-compare liveness leg (the cursor
  blink / prompt arrival must eventually change a dump); `-v` remains
  the dev-tool verify for manual `tapestry-demo` runs. G-4 also adds
  `tools/qmp-sendtext.sh` — QMP `input-send-event` typing on the
  display-bound PCI keyboard (`kbd-pci0`), the `ls-gfx` scenario's
  graphical-input leg.
- **PNG is native** (QEMU ≥ 7.1 `screendump format=png`); `-v` takes a
  PPM sibling dump and asserts the four quadrant-center colors, then
  deletes it.
- **`-c` includes a blend-integrity pass** (G-5; the #35 packed-lane
  class): the bg/fg counts are blind to antialiased EDGE pixels — where
  #35's violet fringing lived while glyph cores stayed exact — so every
  pixel 8-adjacent to an exact-fg core that is neither exact bg nor
  exact fg must sit inside the per-channel `[bg,fg]` envelope (±6); >5%
  outside fails. Legitimate cross-color glyph junctions measure ~2%,
  the #35 formula ~15% for the default fg — the threshold splits both
  with margin. `-F FILE.ppm` runs `-v`/`-c` offline against a saved
  frame (no QMP, no VM); `tools/test-screendump-edge.sh` is the
  standing non-vacuity regression — it synthesizes a frame with the
  literal pre-#35 buggy blend and asserts `-c -F` fails it on the
  blend-integrity arm (and passes the correctly blended twin).
- **Capture requires a LIVE driving Proc.** At driver-Proc reap the RW-7
  proc-death quiesce (`kernel/virtio.c::virtio_mmio_reset_in_range`)
  resets every virtio device in the dying Proc's MMIO window — required
  for DMA soundness (no in-flight device write into freed KObj_DMA
  pages) — and a virtio-gpu reset destroys the host-side resources and
  disables the scanout (blank surface). So the one-shot `usr/virtio-gpu`
  probe's pattern is only capturable during the probe's lifetime; the
  persistent capture target is the G-1 resident driver, and any
  compositor's display dies with its Proc until warden restarts it (the
  TAPESTRY crash contract's visible half).

Since G-3 the gate's scanout owner is tapestryd + its resident tapestry-demo client: the `-v` quadrant assert proves the FULL compositor path (private 9P session -> weave share -> Loom presents), and a liveness leg (two dumps 0.6 s apart must differ -- the demo's plasma animates at the FRAME clock) proves the present loop live. FAIL diagnostics grep `tapestryd:|tapestry-demo:|warden:`.

Exit status: 0 on capture (and pattern match under `-v`); nonzero
otherwise. The tool is safe to run repeatedly against a live VM — it
only reads the console surface.

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
- Boot success: `"Thylacine boot OK"` printed when init signals `SYS_BOOT_COMPLETE`, after its boot-test asserts pass (the boot banner contract — see §10).
- Kernel panic: `"PANIC:"` prefix, followed by the panic message.
- Phase exit criteria: specific strings printed by test code (e.g. `"TIMER: tick 1000"` for the Phase 1 timer test).

The kernel must print a canonical success banner once init's boot-test asserts pass (via `SYS_BOOT_COMPLETE`; see §10). This is not optional — it is the agent's signal that the boot succeeded.

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
tools/build.sh sysroot      ← build the pouch POSIX sysroot (Phase 6 — POUCH-DESIGN.md)
tools/build.sh userspace    ← cargo build --release --workspace for Rust components
tools/build.sh disk         ← assemble build/disk.img (Stratum volume + initramfs)
tools/build.sh all          ← kernel + sysroot + userspace + disk
tools/build.sh all --production ← the lean V1.0 boot shape (no tests, no boot-test probes)
tools/build.sh piboot       ← convert kernel ELF to kernel8.img for Pi 5 netboot (post-v1.0)
tools/test.sh               ← run-vm.sh + test runner (boots VM, runs test suite, reports)
tools/build.sh clean        ← remove build artifacts
```

A top-level `Makefile` provides `make kernel`, `make all`, `make test`, `make production`, etc. as conventional aliases for the above.

### The production boot shape (`--production`, #61)

By default every build is the **dev/CI shape**: the in-kernel test suite is
compiled in and `test_run_all()` runs the whole `g_tests[]` at boot, and joey
(the init) runs its boot-test probe ladder before signalling boot-complete.
That shape is what every gate depends on, but it is NOT what V1.0 ships: the
kernel self-measures a ~525 ms (TCG) / ~1.2 s (HVF) boot-time dominated by the
test suite + the probes (incl. joey's argon2id E2E derivations).

`tools/build.sh --production` (or `make production`) produces the **lean
boot shape**, flipping two CMake options in lockstep:

- `KERNEL_TESTS=OFF` — drops `kernel/test/*.c` from the kernel link and skips
  `test_run_all()` + the boot-stack probe (the banner prints
  `tests: DISABLED (KERNEL_TESTS=OFF production build)`). Kernel flat binary
  929 KiB → 282 KiB.
- `THYLA_BOOT_PROBES=OFF` — compiles joey without its boot-test probe ladder.
  The **real bringup is unconditional**: stratumd mount → `pivot_root` → `/srv`
  re-graft → corvus spawn → `SYS_BOOT_COMPLETE` → console relinquish → the
  login getty. Only the probes (torture/stress/bench + the corvus
  USER_CREATE/AUTH/RECOVER/login E2E ladder) compile out.

Measured: the production shape boots to `Thylacine boot OK` in **~21.5 ms**
(TCG) — vs ~525 ms for the dev build — well under the 500 ms `VISION.md §4.5`
target. The `Thylacine boot OK` banner + the `EXTINCTION:` prefix (the binding
tooling ABI, §10) are byte-identical in both shapes.

**Caveat — first-login provisioning is a seam.** The dev/CI shape's probe
ladder also mints the bootstrap users (michael/susan/cora) as test fixtures.
`--production` does NOT create them, so a real production deployment boots to
the login getty with an empty user set and needs a first-login
account-provisioning UX (owned by the login/Life-Support line). The production
shape is the build + boot-time deliverable; the deployment UX is separable.

Cross-compilation toolchain (installed on host macOS):
- Kernel + native freestanding userspace: `clang` with `--target=aarch64-none-elf`.
- Pouch POSIX userspace (Phase 6): `clang` with `--target=aarch64-thylacine`
  + `--sysroot=build/sysroot`, via `cmake/Toolchain-aarch64-pouch.cmake`
  (CMake projects) or the `tools/pouch-clang` wrapper (plain-Makefile /
  autotools projects — musl, libsodium, stratumd). See `POUCH-DESIGN.md §9`.
- `lld` as the linker.

`tools/build.sh sysroot` creates `build/sysroot/{include,lib}` and runs a
toolchain self-check; the sysroot is populated by Pouch sub-chunks 2-5
(musl headers + libc.a + CRT). See `docs/phase6-status.md`.

The kernel build uses Clang (not GCC) per `ARCHITECTURE.md §3` for CFI + ARMv8.5 feature support.

---

## 10. The boot banner contract

**This is non-negotiable for the agentic loop to work.**

The kernel must print the following banner during boot:

```
Thylacine vX.Y-dev booting...
  arch: arm64
  cpus: N
  mem:  XXXX MiB
  dtb:  0xADDR
  hardening: MMU+W^X+extinction+KASLR+vectors+IRQ+canaries (unconditional); PAC/BTI/LSE conditional
  features:  PAC,BTI,LSE,CRC32 (CPU-implemented)
  kernel base: 0xADDR (KASLR offset 0xADDR)
Thylacine boot OK
```

The multi-line header is printed by `boot_main()` (`kernel/main.c`) during late bring-up, before it enters the init process (joey). The `hardening:` and `features:` lines are **informational** — the agentic-loop tooling matches only `Thylacine boot OK` and the `EXTINCTION:` prefix (below), so these lines are free to evolve. Since Lazarus W1 (`PORTABILITY.md §4`) the `hardening:` line lists the features that hold on **every** target and marks PAC / BTI / LSE **runtime-conditional** (best-effort on capable silicon; absent on the ARMv8.0 floor), while the `features:` line reports what the running CPU actually implements.

The final line `Thylacine boot OK` is the agent's boot-success signal. Since A-5a (login + session), it is printed by `boot_mark_complete()` when **init signals `SYS_BOOT_COMPLETE`** -- after joey's boot-test asserts pass and just before joey transitions to the persistent session supervisor (it getty-loops `/sbin/login`). The banner no longer rides joey's exit: joey is the long-running init and does not exit on success. It must:
- Appear on a line by itself.
- Appear only after init's boot-test asserts have passed (a pre-completion failure exits joey non-zero before `SYS_BOOT_COMPLETE`, which extincts in `joey_run` -> the banner never prints).
- Not appear if the kernel panicked, or if init failed, before signalling boot-complete.

`SYS_BOOT_COMPLETE` is one-shot and gated on the caller being console-attached (the boot console-trust anchor, joey), so a spawned child cannot emit a premature banner.

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
