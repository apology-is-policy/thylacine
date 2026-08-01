---
id: sub-substrate-machine
type: sub
parent: moc-substrate
title: "The machine — run-vm.sh, the QEMU virt board, HVF vs TCG"
code:
  - tools/run-vm.sh
audit: none
guarded-by: []
validated-by: [prose, gate-smp]
locks: []
abis: [abi-boot-banner]
design: ["docs/TOOLING.md", "docs/PORTABILITY.md"]
created: 2026-08-01
updated: 2026-08-01
---
## Purpose

The single canonical QEMU invocation. Every other harness shells out to it,
so the board Thylacine develops against is defined in exactly one file —
"direct `qemu-system-aarch64` invocations diverge and accumulate
inconsistencies" (TOOLING.md §3).

## Contract

- `-machine virt` with the accel-appropriate GIC version; `-smp 4` and
  `-m 2048` by default.
- Boots `thylacine.bin` (the flat image), never the ELF.
- Wires the full device complement: two virtio-blk (pool + scratch disk),
  two NICs (mmio + PCI), keyboard/tablet/mouse, two GPUs, two RNGs, the 9P
  host share, and a QMP control socket.
- Prints `==> qemu: accel=<a> cpu=<c> gic=v<n> smp=<n>` — the line
  downstream harnesses read back rather than re-deriving.
- Every device group is disable-able by env (`THYLACINE_NO_{NET,INPUT,GPU,QMP,SHARE}`).

## Mechanism

**The kernel must be loaded as a flat binary, and that is a QEMU
requirement, not a preference.** QEMU's `load_aarch64_image()` detects the
`ARM\x64` magic at offset 0x38 and treats the file as a Linux Image
(`is_linux=1`), which is what makes it load the DTB and pass its address in
`x0`. An ELF load takes the other branch (`is_linux=0`) and **skips the DTB
entirely** — the guest then boots with `x0 = 0` and no hardware view at all.
The ELF exists only for the debugger.

**Slot allocation is reverse-order, and the tree exploits it deliberately.**
QEMU's virt machine assigns virtio-mmio slots by reading the device list
back-to-front, so the FIRST `-device` on the command line lands in the
highest slot (31). Two virtio-blk devices are wired, and the collision is
resolved by scan direction rather than by configuration:

| Device | Slot | Claimed by | Scan direction |
|---|---|---|---|
| `pool.img` (Stratum) | 31 | stratumd's `bdev_thylacine.c` | HIGH → LOW |
| `disk.img` (scratch) | 30 | `virtio-blk-probe` / `virtio-blk-rw` | LOW → HIGH |

Two devices, two directions, no negotiation and no collision. It works
because each consumer stops at its first match from opposite ends.

**`-device` ORDER is load-bearing for the PCI functions, for a reason that
is not about slots at all.** qemu-virt's PCI INTx has only four shared
lines, assigned `(slot + pin) % 4`, and `KObj_IRQ` is exclusive per INTID —
so the two IRQ-CLAIMING functions (the PCI NIC → netd, the GPU →
tapestryd) must land on distinct lines. Inserting the relative mouse before
the GPU shifted the GPU one PCI slot onto the NIC's line (intid 35 → 36)
and its `SYS_IRQ_CREATE` failed on exclusivity. The input functions are
poll-mode and never claim an IRQ, so the mouse can share any line — which
is why it expands LAST, after gpu and rng.

**The G-1 co-page rule is why persistent drivers are on PCI.** QEMU-virt
packs every populated MMIO slot into ONE 4 KiB page (stride 0x200), and
userspace MMIO claims are page-granular and exclusive. So that page belongs
temporally to the transient probes and then permanently to stratumd
(virtio-blk); a second persistent claimant starves the disk — boot-fatal,
measured at G-1. PCI BARs are per-function with no co-residency, so every
resident driver (tapestryd's GPU + keyboard + tablet, netd's NIC) takes a
PCI function while the one-shot kernel-test probes keep the MMIO devices.
Hence the deliberate `gpu0`/`gpu-mmio0` and `kbd-pci0`/`kbd0` pairs.

**The accel matrix is two coherent configurations, not a knob.**

| | accel | `-cpu` | GIC | Why |
|---|---|---|---|---|
| default on Apple Silicon | `hvf` | `host` | v2 | HVF's emulated GICv3 distributor MMIO trips an `isv` data-abort assert; the GICv2 MMIO CPU interface is the HVF enabler (Lazarus W2) |
| fallback / compat | `tcg` | `max` | v3 | full ISA incl. RNDR; v3 is QEMU-virt's modern default |

Auto-detection probes the host (`kern.hv_support`) AND the qemu build, so
the launcher still works on a non-Apple box. The kernel autodetects v2-vs-v3
from the DTB, so neither choice is baked into the guest. Under HVF the guest
sees Apple cores, which have LSE+PAC+BTI but **not** FEAT_RNG/RNDR — which
is precisely why the kernel CSPRNG seeds from virtio-rng (Lazarus W3): one
software path on every target.

**The `nowatchpoint` token, and why its ABSENCE is the safe default.** QEMU
TCG programs `DBGWVR`/`DBGWCR` but never raises EC 0x34; a guest thread that
touches a watched page then spins inside the emulator's retry of that one
instruction, takes no timer IRQ, never reaches the EL0-return tail, and
therefore **cannot be killed** — under TCG's round-robin that wedged vCPU
starves the guest and the boot never finishes. The kernel is correct (the
same encoding fires on real silicon under HVF), so the only safe move is to
not arm a watchpoint on that substrate. It is advertised by appending
`thylacine.nowatchpoint` to `/chosen/bootargs`, which the guest reads back
through the `/hw` FDT mount — **no kernel cmdline parser required**. The
polarity is the design: absence means "watchpoints work", so every
substrate that is not TCG keeps the hard assertion, and `test.sh` enforces
the fire on any accel that can deliver it. A new substrate cannot silently
inherit the exemption.

**The vnc display drops the MMIO GPU.** A display backend binds QemuConsole
0, and `gpu-mmio0` (probe-only, driverless in a resident boot) would squat
it — the VNC client must land on `gpu0`'s head. `cocoa` keeps the canonical
device set because its View menu switches consoles interactively.

## Data structures

None. The script's state is bash arrays expanded with the
`${arr[@]+"${arr[@]}"}` idiom, which is required for empty-array expansion
under `set -u`.

## Concurrency

One VM per invocation. Concurrency lives in the callers: two worktrees can
run VMs simultaneously, which is why every reap in this area must be scoped
to its own build dir ([[sub-substrate-interactive]], #59).

## Invariants enforced

None of §28 — this is host tooling. It does however *establish the
conditions* several kernel invariants are verified under: I-5's hardware
exclusivity is what the INTx ordering rule serves, and the GICv2 selection
is what makes the I-18 IPI path reachable under HVF at all.

## Error paths

Missing `thylacine.bin` → refuse with a build hint. Unknown
`THYLACINE_DISPLAY` → exit 2. A missing pool/disk/ramfs is a *soft* absence:
the flag group is simply omitted and the guest degrades (no
`/srv/stratum-fs`), which is why run-vm announces which pool it is booting.

## Performance

HVF boots far faster than TCG; the 90 s default timeout in `test.sh` is
sized for the slower TCG compat run. Idle cost under HVF was the subject of
#299/#890 (the tickless work) — a never-stopped tick showed 332% idle.

## Prosecution

- The two-scan-direction pool/disk split depends on QEMU's reverse-order
  slot assignment. A QEMU change here silently swaps which device stratumd
  mounts.
- Any new `-device` inserted before `gpu_flags` re-shuffles PCI slots and
  can collide two IRQ-claiming functions onto one INTx line. Add at the end.
- A new accel or board must either deliver EL0 watchpoints or set
  `thylacine.nowatchpoint`; inheriting the exemption by accident is the
  failure the absence-default is designed to prevent.
- `-cpu host` under HVF means the gate's CPU is the *dev machine's* CPU —
  LSE is present, so this path is structurally blind to an ARMv8.0 floor
  regression. That is [[gate-v80-floor]]'s job.

## Seams

[[seam-70-tcg-watchpoint]] · [[seam-791-smp1-joey]].

## Caveats

- `--snapshot` is parsed but unimplemented: it prints "not yet implemented"
  and continues. TOOLING.md §6 describes the snapshot workflow as a
  Phase-5+ deliverable; the flag is a placeholder, not a feature.
- The header comment still describes P1-A ("kernel boots, prints banner,
  hangs. No disk image yet... uncomment as the corresponding subsystems
  land") and predicts graphical flags "at Phase 8". Both landed long ago and
  the flags below the comment are live — read the code, not the preamble.
- The pool-size line is a *coarse tell* (64M bootstrap / 2560M goroot /
  3072M clade / 5120M both), explicitly **not** a verification. The
  verification is at the bake chokepoint ([[sub-substrate-build]], #101).

## Provenance

[[chg-2026-08-01-substrate-sweep]] records the sweep. The board accreted
across the whole project; the load-bearing corrections are noted inline
above (G-1 co-page, the INTx ordering, Lazarus W2/W3, task #70).
